// codeharbord (SPEC 10): the remote workspace service. Launched over SSH as
// `codeharbord rpc --stdio` and speaks newline-delimited JSON-RPC 2.0 on
// stdin/stdout. It need not run permanently.
//
// Bootstrap scope: request framing, dispatch, and two introspection methods so
// the transport can be exercised end-to-end. Workspace DB, file, watch, tmux,
// and agent methods (SPEC 10.2) land in the workstreams in docs/PLAN.md.

// pathToFileURL, not string concatenation: process.argv[1] is a filesystem
// path, and a path containing a space (or any character a URL must
// percent-encode) would never compare equal to import.meta.url, silently
// turning the CLI entry point below into a no-op.
import { pathToFileURL } from "node:url";
import { homedir } from "node:os";
import { join as pathJoin } from "node:path";

// Frozen file-method catalog (C1, docs/PLAN.md). RPC_METHODS/RPC_REVISION_MISMATCH
// are re-exported so the wire names and error code stay linked to the transport.
// The file.* handlers (incl. listDirectory) are registered from files.ts.
import { fileMethods, fileWatchService, isResourceLimit, isRevisionMismatch } from "./files.ts";
// Workspace persistence method group (workstream P). `workspace.*` is P's own
// method group and is deliberately absent from the frozen C1 file catalog.
// serverIdentity() is this host's stable, persisted id, reported by server.info.
// isDatabaseBusy recognizes SQLite's lock-contention failures, which are a
// transient race between codeharbord processes sharing one database file, not
// a server malfunction — see the dispatch() branch below.
import { isDatabaseBusy, serverIdentity, WORKSPACE_METHODS } from "./workspace.ts";
// tmux session discovery (SPEC 10.2). Its own `tmux.*` method group, likewise
// outside the frozen C1 file catalog.
import { TMUX_METHODS } from "./tmux.ts";
// isInvalidParams recognizes the tagged error the param guards throw, so a
// malformed request is answered with "Invalid params" instead of a generic
// internal error that blames the server for the client's bad payload.
import { isInvalidParams } from "./validate.ts";
import {
    RPC_DATABASE_BUSY,
    RPC_INTERNAL_ERROR,
    RPC_INVALID_PARAMS,
    RPC_INVALID_REQUEST,
    RPC_METHOD_NOT_FOUND,
    RPC_PARSE_ERROR,
    RPC_PING_METHOD,
    RPC_RESOURCE_LIMIT,
    RPC_REVISION_MISMATCH,
    RPC_SERVER_INFO_METHOD,
    RPC_WATCH_EVENT_NOTIFICATION,
    RPC_WATCH_EVENTS_LOST_NOTIFICATION,
} from "./rpc-types.ts";
import type { WatchEvent } from "./rpc-types.ts";
export { RPC_METHODS } from "./rpc-types.ts";
// JSON-RPC 2.0 reserved error codes are DEFINED in rpc-types.ts (see the note
// there on the import cycle) and re-exported here, which is where every
// consumer has always imported them from.
export {
    RPC_PARSE_ERROR,
    RPC_INVALID_REQUEST,
    RPC_METHOD_NOT_FOUND,
    RPC_INVALID_PARAMS,
    RPC_INTERNAL_ERROR,
} from "./rpc-types.ts";
export {
    RPC_DATABASE_BUSY,
    RPC_RESOURCE_LIMIT,
    RPC_REVISION_MISMATCH,
    RPC_WATCH_EVENT_NOTIFICATION,
    RPC_WATCH_EVENTS_LOST_NOTIFICATION,
};

export const RPC_SERVER_NAME = "codeharbord";
// The release version, reported by server.info and shown to the USER verbatim
// in the client's "Server too old: codeharbord <version> speaks ..." message
// (AppController). It must match remote/package.json, which the release script
// (.omp/skills/bump-version/bump.sh) rewrites; this constant is NOT one of that
// script's version files, which is how it sat at 0.1.0 while the tag said
// 0.1.8 and every server reported a version three releases stale.
export const RPC_SERVER_VERSION = "0.1.16";
// Bumped 1 -> 2 when file.listDirectory joined the C1 catalog (SPEC 7.5).
// Bumped 2 -> 3 when the tmux.* discovery group joined the catalog (SPEC 10.2).
// Bumped 3 -> 4 when server.info gained `serverId` (SPEC 3.5).
// Bumped 4 -> 5 when workspace.resolveTerminalPane joined the workspace group:
// the client cannot attach a terminal without it (SPEC 5.2), so an older server
// must fail the compatibility gate rather than serve a client that will ask.
// Bumped 5 -> 6 when workspace.resolveTerminalPane began addressing a pane by
// its ROW ID rather than by a layout slot label: a v5 server requires `name`
// and rejects the `id` this client sends, so every terminal would fail to
// attach with an invalid-params error instead of at the compatibility gate.
export const RPC_SCHEMA_VERSION = 6;

export interface RpcRequest {
    jsonrpc: "2.0";
    // Absent (undefined) marks a JSON-RPC notification: no response is returned.
    id?: string | number | null;
    method: string;
    params?: unknown;
}

export interface RpcSuccess {
    jsonrpc: "2.0";
    id: string | number | null;
    result: unknown;
}

export interface RpcError {
    jsonrpc: "2.0";
    id: string | number | null;
    error: { code: number; message: string; data?: unknown };
}

export type RpcResponse = RpcSuccess | RpcError;

// Upper bound on one input line before the transport is dropped, mirroring the
// C++ client's `kMaxLineBytes` in src/remote/CodeharbordClient.cpp (C3): both
// ends of this wire protocol must agree that a frame past 16 MiB is a fault,
// not something to buffer without limit. A parity test in
// remote/test/rpc-mirror.test.ts pins this to the C++ constant.
export const MAX_LINE_BYTES = 16 * 1024 * 1024;

type MethodHandler = (params: unknown) => unknown | Promise<unknown>;

// Static method table (SPEC 10.2 methods are added here as they land).
const methods: Record<string, MethodHandler> = {
    // Transport keepalive (RPC_PING_METHOD). Deliberately the cheapest possible
    // handler: the C++ client's heartbeat drops the transport when several
    // consecutive probes go unanswered, so this must never touch the filesystem
    // or the database — an answer is meant to prove only that this process is
    // still reading its input and writing its output.
    [RPC_PING_METHOD]: () => ({ pong: true }),
    // `name`, `version` and `schemaVersion` are frozen; `serverId` (SPEC 3.5)
    // is the stable identity of the workspace database on THIS host, minted on
    // first read and unchanged thereafter. Clients key their view of the remote
    // workspace by it, so it is read from the DB rather than synthesized: an
    // id that changed per process or per route would orphan every stored row.
    //
    // `recoveryDir` (SPEC 11.3) is the REMOTE user's data directory for
    // crash-recovery snapshots, reported so the client writes them to a location
    // that is valid ON THE SERVER — the client's own data path would be
    // meaningless across an SSH boundary to a different host/user. The client
    // appends its per-pane id; writeFile creates the directory (files.ts) at
    // 0o700 on the first snapshot. Additive and OPTIONAL: it does not raise the
    // schemaVersion floor, so an older server simply omits it and the client
    // degrades to no-recovery rather than failing the compatibility gate.
    [RPC_SERVER_INFO_METHOD]: () => ({
        name: RPC_SERVER_NAME,
        version: RPC_SERVER_VERSION,
        schemaVersion: RPC_SCHEMA_VERSION,
        serverId: serverIdentity(),
        recoveryDir: pathJoin(
            process.env.XDG_DATA_HOME && process.env.XDG_DATA_HOME.length > 0
                ? process.env.XDG_DATA_HOME
                : pathJoin(homedir(), ".local", "share"),
            "codeharbor",
            "recovery",
        ),
    }),
    ...fileMethods,
    ...WORKSPACE_METHODS,
    ...TMUX_METHODS,
};

function isRpcRequest(value: unknown): value is RpcRequest {
    if (typeof value !== "object" || value === null) return false;
    const r = value as Record<string, unknown>;
    return (
        r.jsonrpc === "2.0" &&
        typeof r.method === "string" &&
        (r.id === undefined || typeof r.id === "string" || typeof r.id === "number" || r.id === null)
    );
}

/**
 * Dispatch one already-decoded request object. Pure and total: unknown methods
 * and malformed requests produce JSON-RPC error responses rather than throwing.
 *
 * Batches (a JSON array of request objects) are deliberately NOT supported: the
 * only client is the C++ CodeharbordClient, which writes exactly one request
 * per line and correlates replies by id. An array therefore fails
 * isRpcRequest() and is answered with a single Invalid Request rather than an
 * array of responses.
 */
export async function dispatch(value: unknown): Promise<RpcResponse | null> {
    if (!isRpcRequest(value)) {
        return {
            jsonrpc: "2.0",
            id: null,
            error: { code: RPC_INVALID_REQUEST, message: "Invalid Request" },
        };
    }
    const handler = methods[value.method];
    // JSON-RPC 2.0: `params`, when present, MUST be a structured value — an
    // object or an array. Every handler immediately treats it as a record, so a
    // primitive (or null) is rejected here as Invalid params instead of
    // surfacing later as a confusing internal TypeError from deep in a handler.
    const paramsOk =
        value.params === undefined ||
        (typeof value.params === "object" && value.params !== null);
    // A request with no id is a JSON-RPC notification: dispatch it for its side
    // effects and return NO response, regardless of outcome. Narrowing on
    // `value.id` here also lets the response branches below type `id` as present.
    if (value.id === undefined) {
        if (handler && paramsOk) {
            try {
                await handler(value.params);
            } catch {
                // Notifications get no response, so swallow handler errors.
            }
        }
        return null;
    }
    const id = value.id;
    if (!handler) {
        return {
            jsonrpc: "2.0",
            id,
            error: { code: RPC_METHOD_NOT_FOUND, message: `Method not found: ${value.method}` },
        };
    }
    if (!paramsOk) {
        return {
            jsonrpc: "2.0",
            id,
            error: {
                code: RPC_INVALID_PARAMS,
                message: `Invalid params for ${value.method}: expected an object or array`,
            },
        };
    }
    try {
        const result = await handler(value.params);
        // JSON-RPC 2.0 requires exactly one of `result`/`error` on a response.
        // `undefined` is dropped by JSON.stringify, which would put a response
        // carrying NEITHER member on the wire, so a handler with no payload
        // reports null instead.
        return { jsonrpc: "2.0", id, result: result === undefined ? null : result };
    } catch (err) {
        if (isRevisionMismatch(err)) {
            return {
                jsonrpc: "2.0",
                id,
                error: { code: RPC_REVISION_MISMATCH, message: err.message, data: err.data },
            };
        }
        // A param guard rejected the payload (validate.ts). That is a fault in
        // the REQUEST, so it must not be reported as -32603 "internal error",
        // which tells the client the SERVER broke and hides the real cause from
        // whoever has to debug it.
        if (isInvalidParams(err)) {
            return {
                jsonrpc: "2.0",
                id,
                error: { code: RPC_INVALID_PARAMS, message: err.message },
            };
        }
        // A valid request whose answer would blow a server bound (a directory
        // listing too big for one transport frame, or one watch too many). Not
        // -32603 for the same reason as the busy code below: nothing
        // malfunctioned and nothing was half-applied. The message already names
        // the limit and what to do, and the client shows it verbatim.
        if (isResourceLimit(err)) {
            return {
                jsonrpc: "2.0",
                id,
                error: { code: RPC_RESOURCE_LIMIT, message: err.message },
            };
        }
        // SQLite could not take the write lock before workspace.ts's busy
        // timeout ran out. Like the guard above this is NOT -32603: the server
        // is healthy, nothing was applied, and another codeharbord process
        // sharing this database file simply held the lock longer than we were
        // willing to wait. Telling the client "internal error" would send a
        // user hunting a server fault over a request they can just repeat.
        if (isDatabaseBusy(err)) {
            return {
                jsonrpc: "2.0",
                id,
                error: {
                    // The client has no special case for this code and shows
                    // this text to the USER verbatim, so it must be an
                    // instruction rather than SQLite's "database is locked" —
                    // a driver string that reads like corruption to anyone who
                    // does not know it means "someone else is writing".
                    code: RPC_DATABASE_BUSY,
                    message:
                        "The workspace database is busy: another client is writing to it. " +
                        "Nothing was changed — please try again.",
                },
            };
        }
        return {
            jsonrpc: "2.0",
            id,
            error: {
                code: RPC_INTERNAL_ERROR,
                message: err instanceof Error ? err.message : String(err),
            },
        };
    }
}

/** Handle one raw JSONL request line, returning the response to serialize. */
export async function handleLine(line: string): Promise<RpcResponse | null> {
    let decoded: unknown;
    try {
        decoded = JSON.parse(line);
    } catch {
        return {
            jsonrpc: "2.0",
            id: null,
            error: { code: RPC_PARSE_ERROR, message: "Parse error" },
        };
    }
    return dispatch(decoded);
}

/**
 * Hand-written line framer mirroring the C++ client's bounded reader
 * (kMaxLineBytes in src/remote/CodeharbordClient.cpp, C3). Emits each
 * newline-delimited line via `onLine`, and calls `onOverflow` once the bytes
 * held for an unfinished line pass MAX_LINE_BYTES — the signal to drop the
 * connection rather than buffer a frame without bound. Node's readline owns its
 * buffer and offers no maximum line option, so the framing is done by hand
 * here. Returns the per-chunk feed.
 *
 * Incomplete lines are held as a LIST of chunk slices and joined only when the
 * newline arrives. Appending each chunk to one growing Buffer instead (a
 * Buffer.concat per chunk) copies everything received so far on every chunk, so
 * a 16 MiB frame — which is a routine file.writeFile of a large file, base64 —
 * arriving in 64 KiB reads costs gigabytes of memcpy for a megabytes-sized
 * request. The join is one copy of the finished line.
 *
 * After an overflow the framer RESYNCHRONIZES: everything up to the next
 * newline is discarded instead of being framed as a line of its own. Emitting
 * the tail of an oversized frame would hand the dispatcher an arbitrary slice
 * of a payload the peer never finished — parsed as if it were a request the
 * peer had actually sent — and would fire `onOverflow` again for every further
 * MAX_LINE_BYTES of the same frame.
 */
export function createLineFramer(
    onLine: (line: string) => void,
    onOverflow: () => void,
): (chunk: Buffer) => void {
    let held: Buffer[] = [];
    let heldBytes = 0;
    // True between an overflow and the newline that ends the offending frame.
    let discarding = false;
    return (chunk) => {
        let start = 0;
        let nl = chunk.indexOf(0x0a);
        while (nl !== -1) {
            const segment = chunk.subarray(start, nl);
            if (discarding) {
                // The end of the over-cap frame: drop it and resume framing.
                discarding = false;
            } else {
                const lineBytes = heldBytes + segment.length;
                if (lineBytes > MAX_LINE_BYTES) {
                    // The newline is in this chunk, so the offending frame is
                    // complete already: report it and discard the whole line,
                    // but resume framing immediately after this newline.
                    held = [];
                    heldBytes = 0;
                    onOverflow();
                } else if (heldBytes === 0) {
                    onLine(segment.toString("utf8"));
                } else {
                    held.push(segment);
                    onLine(Buffer.concat(held, lineBytes).toString("utf8"));
                }
            }
            held = [];
            heldBytes = 0;
            start = nl + 1;
            nl = chunk.indexOf(0x0a, start);
        }
        // Still inside the over-cap frame: this whole chunk is part of it.
        if (discarding) return;
        if (start < chunk.length) {
            const rest = chunk.subarray(start);
            held.push(rest);
            heldBytes += rest.length;
        }
        // Past the cap with no newline: a peer is streaming an unbounded frame.
        // Checking the held byte count here bounds memory to roughly one chunk
        // beyond the cap, unlike a post-line length check that only fires once
        // the whole oversized line is already resident.
        if (heldBytes > MAX_LINE_BYTES) {
            held = [];
            heldBytes = 0;
            discarding = true;
            onOverflow();
        }
    };
}

// Bound on the watch notifications the relay below may hold while the client's
// end of the SSH channel is stalled.
//
// A watch notification is small and fixed in shape: a subscription id
// ("sub-<n>-<8 hex>", ~20 bytes), the watched path, an event kind, and an
// optional revision token (~40 bytes) — so its serialized line is bounded by
// the path, and a path is bounded by PATH_MAX (4096). 512 entries is therefore
// at most ~2 MiB of pathological long paths and a few tens of KiB in practice.
//
// The count is also generous against what can actually accumulate: the relay
// coalesces per (subscription, path), and a subscription watches exactly ONE
// path (FileWatchService.watch in files.ts), so the queue cannot exceed the
// number of LIVE subscriptions no matter how furiously the filesystem churns.
// A build writing thousands of files under one watched path collapses to one
// entry. Reaching 512 means the client opened more than 512 watches, not that
// the filesystem is busy.
//
// The byte cap is the second, independent guard: it binds first when paths are
// long, so the memory ceiling holds even if the count ceiling does not bite.
export const MAX_PENDING_WATCH_EVENTS = 512;
export const MAX_PENDING_WATCH_BYTES = 1024 * 1024;

export interface WatchNotificationRelay {
    // Offer one watch event for delivery. Written straight through when the
    // output is flowing; queued (and coalesced) while it is stalled.
    deliver(event: WatchEvent): void;
    // Forget everything held for a subscription the service has released.
    forget(subscriptionId: string): void;
    // Report that SOMEONE ELSE's write to the same stream reported a full
    // buffer. The relay shares `out` with the RPC response writer, and only
    // sees the return value of its own writes; without this a response that
    // stalled the stream would leave the relay believing the output is still
    // flowing, so every subsequent notification would be written straight into
    // an internal buffer with no bound — the exact failure the queue below
    // exists to prevent. Cleared by the stream's next 'drain'.
    stall(): void;
    // Queue depth, for tests asserting the bound actually holds.
    pendingCount(): number;
}

/**
 * Relay watch events to `out` as id-less JSON-RPC notifications, with a BOUNDED
 * queue and explicit reporting of anything it had to drop.
 *
 * Backpressure has the same shape as the bridge program's makeStreamSink
 * (remote/src/bridge.ts): a write returning false marks the output stalled, and
 * the next 'drain' resumes it. The difference is that the bridge can pause its
 * producer sockets and this cannot — the producer is the filesystem. So instead
 * of pausing, pending notifications are held in a map keyed by (subscription,
 * path) and COALESCED: a later event for a key replaces the earlier one.
 *
 * Coalescing is information-preserving here. A watch notification only tells
 * the client "this path changed, re-read it": EditorController::onNotification
 * (src/editor/EditorController.cpp) ignores the event KIND entirely and either
 * flags the pane (dirty buffer) or re-reads the file (clean buffer). Keeping
 * the LATEST event per key is what makes that exact: its `revision` is the
 * current on-disk one, which is also what lets the client's own-write echo
 * suppression still fire. An older event in its place would force a needless
 * re-read; N events in its place would force N.
 *
 * Past the bound, events are genuinely dropped — and the client is TOLD, via a
 * file.watchEventsLost notification naming the affected subscriptions, because
 * a client that silently misses a change displays stale content while believing
 * it is current. Only subscription ids are retained for that (a fixed ~20 bytes
 * each, bounded by the live subscription count the client itself created), so
 * the loss record cannot become the unbounded buffer it exists to prevent.
 */
export function createWatchNotificationRelay(
    out: NodeJS.WritableStream,
    hasSubscription: (subscriptionId: string) => boolean,
    onStall: () => void = () => {},
    onDrain: () => void = () => {},
): WatchNotificationRelay {
    // The queued entry holds the SERIALIZED line, not the event: it is built
    // once (its byte length is what the byte bound measures) and written
    // verbatim on drain, instead of stringifying the same event twice.
    interface Pending {
        subscriptionId: string;
        line: string;
        bytes: number;
    }
    const pending = new Map<string, Pending>();
    // Subscriptions for which at least one event was dropped and the client has
    // not yet been told.
    const lost = new Set<string>();
    let pendingBytes = 0;
    let stalled = false;

    const notificationLine = (event: WatchEvent): string =>
        `${JSON.stringify({
            jsonrpc: "2.0",
            method: RPC_WATCH_EVENT_NOTIFICATION,
            params: event,
        })}\n`;
    const writeLine = (line: string): void => {
        // A false return means Node is now buffering internally; everything
        // after it must queue instead of adding to that buffer.
        if (!out.write(line)) {
            stalled = true;
            onStall();
        }
    };
    const flushLost = (): void => {
        if (stalled || lost.size === 0) return;
        // A subscription released while its loss was pending has no consumer
        // left to inform, so it is dropped rather than reported.
        const subscriptionIds = [...lost].filter(hasSubscription);
        lost.clear();
        if (subscriptionIds.length === 0) return;
        writeLine(
            `${JSON.stringify({
                jsonrpc: "2.0",
                method: RPC_WATCH_EVENTS_LOST_NOTIFICATION,
                params: { subscriptionIds },
            })}\n`,
        );
    };

    out.on("drain", () => {
        onDrain();
        stalled = false;
        for (const [key, entry] of pending) {
            // Delete before writing: a write that re-stalls must leave the
            // REMAINING entries queued, not re-send this one on the next drain.
            pending.delete(key);
            pendingBytes -= entry.bytes;
            if (hasSubscription(entry.subscriptionId)) writeLine(entry.line);
            if (stalled) return;
        }
        flushLost();
    });

    return {
        deliver(event) {
            if (!hasSubscription(event.subscriptionId)) return;
            const line = notificationLine(event);
            if (!stalled) {
                writeLine(line);
                // The write may have just stalled the output; the loss report
                // then waits for drain like everything else.
                flushLost();
                return;
            }
            const key = `${event.subscriptionId}\u0000${event.path}`;
            const bytes = Buffer.byteLength(line);
            const entry: Pending = { subscriptionId: event.subscriptionId, line, bytes };
            const existing = pending.get(key);
            if (existing) {
                // Coalescing still has to honor the byte cap: replacing a
                // small entry with a larger one can otherwise push the queue
                // beyond MAX_PENDING_WATCH_BYTES without increasing its count.
                const nextBytes = pendingBytes + bytes - existing.bytes;
                if (nextBytes > MAX_PENDING_WATCH_BYTES) {
                    pending.delete(key);
                    pendingBytes -= existing.bytes;
                    lost.add(event.subscriptionId);
                    return;
                }
                // Coalesce: the newest event for this key supersedes the one
                // held. Map preserves the original insertion position, which
                // keeps the flush order stable.
                pendingBytes = nextBytes;
                pending.set(key, entry);
                return;
            }
            if (
                pending.size >= MAX_PENDING_WATCH_EVENTS ||
                pendingBytes + bytes > MAX_PENDING_WATCH_BYTES
            ) {
                lost.add(event.subscriptionId);
                return;
            }
            pending.set(key, entry);
            pendingBytes += bytes;
        },
        forget(subscriptionId) {
            for (const [key, entry] of pending) {
                if (entry.subscriptionId !== subscriptionId) continue;
                pending.delete(key);
                pendingBytes -= entry.bytes;
            }
            lost.delete(subscriptionId);
        },
        stall() {
            stalled = true;
            onStall();
        },
        pendingCount: () => pending.size,
    };
}

// Signals the daemon treats as "stop cleanly". SIGHUP is in the set because it
// is what an ending SSH session delivers to the process it launched, which is
// this daemon's most common way to be told to stop; SIGINT covers a hand-started
// daemon under Ctrl-C; SIGTERM covers `kill` and every supervisor.
export const SHUTDOWN_SIGNALS = ["SIGTERM", "SIGINT", "SIGHUP"] as const;
// Responses cannot be dropped like watch notifications: the client would
// wait forever for the matching request id. Queue them while stdout is stalled,
// but close the transport once the queue itself reaches a bounded size rather
// than letting Node's internal writable buffer grow without limit.
export const MAX_PENDING_RPC_RESPONSES = 512;
export const MAX_PENDING_RPC_RESPONSE_BYTES = MAX_LINE_BYTES;


// How long the shutdown backstop waits for in-flight work before forcing the
// exit. Two seconds is beyond any handler that is making progress and well
// inside the patience of a supervisor's own SIGTERM-then-SIGKILL window.
export const SHUTDOWN_GRACE_MS = 2000;
export interface StdioHandle {
    /**
     * Run the orderly shutdown now, exactly as an incoming SIGTERM would.
     * Idempotent: every call after the first does nothing. Exposed so a test can
     * exercise the path without signalling its own process.
     */
    shutdown: () => void;
}

export function runStdio(): StdioHandle {
    // The client half of the SSH channel can disappear at any moment, and the
    // next write then raises EPIPE as an 'error' event on stdout. Unhandled,
    // that event terminates the process with a stack trace; swallow it and let
    // the stdin 'close' handler below perform the orderly shutdown. Swallow
    // stdin errors too: destroying the stream on an over-cap frame (below) must
    // not surface as an unhandled 'error'.
    process.stdout.on("error", () => {});
    process.stdin.on("error", () => {});
    // Relay watch notifications to the client as id-less JSON-RPC messages.
    // The relay's queue is bounded because filesystem producers cannot pause.
    // Responses use a separate bounded queue below because they cannot be
    // dropped: the client would otherwise wait forever for their ids.
    let outputStalled = false;
    let outputFailed = false;
    const pendingResponses: string[] = [];
    let pendingResponseBytes = 0;
    let pendingResponseIndex = 0;
    const watchRelay = createWatchNotificationRelay(
        process.stdout,
        (id) => fileWatchService.hasSubscription(id),
        () => {
            outputStalled = true;
        },
        () => {
            outputStalled = false;
        },
    );
    fileWatchService.onWatchEvent((event) => watchRelay.deliver(event));
    // No queue outlives its subscriber: unwatch, and closeAll on stdin close,
    // both announce the release here.
    fileWatchService.onWatchClosed((id) => watchRelay.forget(id));
    // CONCURRENCY AND ORDERING, stated plainly because the wire cannot show it:
    //
    // Each input line starts its handler IMMEDIATELY, without awaiting the
    // previous one. Responses are therefore written in COMPLETION order, not in
    // arrival order: a slow file.readFile of a large file can be answered after
    // a later, faster file.stat. JSON-RPC 2.0 explicitly allows this, and the
    // client matches replies by id rather than stream position. The input
    // producer is trusted to keep the number of active calls reasonable; the
    // output queue itself is bounded so a stalled peer cannot grow Node's
    // writable buffer without limit.
    const failOutput = (): void => {
        if (outputFailed) return;
        outputFailed = true;
        outputStalled = true;
        pendingResponses.length = 0;
        pendingResponseBytes = 0;
        pendingResponseIndex = 0;
        fileWatchService.closeAll();
        process.exitCode = 1;
        if (!process.stdin.destroyed) process.stdin.destroy();
    };
    const flushResponses = (): void => {
        if (outputFailed) return;
        while (pendingResponseIndex < pendingResponses.length) {
            const line = pendingResponses[pendingResponseIndex++];
            pendingResponseBytes -= Buffer.byteLength(line);
            try {
                if (!process.stdout.write(line)) {
                    outputStalled = true;
                    watchRelay.stall();
                    break;
                }
            } catch {
                failOutput();
                return;
            }
        }
        if (pendingResponseIndex === pendingResponses.length) {
            pendingResponses.length = 0;
            pendingResponseIndex = 0;
            pendingResponseBytes = 0;
        }
    };
    process.stdout.on("drain", flushResponses);
    process.stdout.on("error", failOutput);
    const writeOut = (line: string): void => {
        if (outputFailed) return;
        const bytes = Buffer.byteLength(line);
        // Sending a response larger than the framing cap would make the peer
        // drop the entire transport, so fail before writing a doomed frame.
        if (bytes > MAX_LINE_BYTES) {
            failOutput();
            return;
        }
        if (outputStalled) {
            const queuedCount = pendingResponses.length - pendingResponseIndex;
            if (
                queuedCount >= MAX_PENDING_RPC_RESPONSES ||
                pendingResponseBytes + bytes > MAX_PENDING_RPC_RESPONSE_BYTES
            ) {
                failOutput();
                return;
            }
            pendingResponses.push(line);
            pendingResponseBytes += bytes;
            return;
        }
        try {
            if (!process.stdout.write(line)) {
                outputStalled = true;
                watchRelay.stall();
            }
        } catch {
            failOutput();
        }
    };
    const dispatchLine = (line: string): void => {
        // Skip separator lines without copying the line: trim() on a multi-MiB
        // base64 file.writeFile frame duplicates the whole string just to answer
        // "is this blank?". The C++ reader avoids the same copy deliberately.
        if (!/\S/.test(line)) return;
        void handleLine(line)
            .then((response) => {
                if (!response) return;
                try {
                    writeOut(`${JSON.stringify(response)}\n`);
                } catch {
                    // Only a non-serializable handler payload (a reference
                    // cycle, or a BigInt) reaches here. Answer the request with
                    // an internal error so the client's pending call completes
                    // instead of waiting forever for a line that cannot be
                    // produced.
                    writeOut(
                        `${JSON.stringify({
                            jsonrpc: "2.0",
                            id: response.id,
                            error: {
                                code: RPC_INTERNAL_ERROR,
                                message: "Response could not be serialized",
                            },
                        })}\n`,
                    );
                }
            })
            .catch(() => {
                // handleLine is total today. This guard exists because an
                // unhandled promise rejection is fatal by default in Node: one
                // future non-total code path would take the whole service — and
                // every live watch subscription — down with it.
                writeOut(
                    `${JSON.stringify({
                        jsonrpc: "2.0",
                        id: null,
                        error: { code: RPC_INTERNAL_ERROR, message: "Internal error" },
                    })}\n`,
                );
            });
    };
    // Replaces Node's readline, which buffers a line without any upper bound
    // (RR13). The framer drops the connection past MAX_LINE_BYTES.
    const feed = createLineFramer(dispatchLine, () => {
        process.stderr.write(
            `codeharbord: input line exceeded ${MAX_LINE_BYTES} bytes without a newline; closing connection\n`,
        );
        // The transport can no longer be trusted; end the input stream. The
        // 'close' handler below then releases watch handles so the process can
        // exit once in-flight work settles.
        process.stdin.destroy();
    });
    process.stdin.on("data", feed);
    // SIGNALS. Until this existed, the ONLY orderly shutdown was stdin closing:
    // a plain `kill` (or the SIGHUP an ending SSH session delivers, or Ctrl-C on
    // a hand-started daemon) hit Node's default disposition and killed the
    // process where it stood. That left the daemon's observable exit status as
    // "died on a signal" rather than "exited 0", which is what a supervisor or a
    // packaging script has to distinguish a clean stop from a crash-loop; it
    // abandoned every fs.watch handle and poll timer instead of releasing them;
    // and it discarded a response already handed to a stdout the SSH channel had
    // not drained yet, so the client's pending call never completed.
    //
    // The handler runs the SAME path as stdin close and is IDEMPOTENT: a second
    // signal (a supervisor's SIGTERM followed by an impatient one) does nothing
    // at all. The listeners stay REGISTERED for that reason — deregistering them
    // restores Node's default disposition, so the impatient second SIGTERM would
    // kill the process mid-shutdown and turn the clean exit back into the
    // signal death this exists to remove. Keeping them costs nothing: a signal
    // listener does not hold the event loop open, so the process still exits by
    // itself once the work below has released everything.
    let shuttingDown = false;
    const shutdown = (): void => {
        if (shuttingDown) return;
        shuttingDown = true;
        // Releases every OS watch handle and poll timer, and announces each
        // release so the relay drops its queued notifications with it.
        fileWatchService.closeAll();
        // Stop reading requests. Answering a new one while shutting down would
        // start work whose reply cannot be guaranteed to reach the wire.
        process.stdin.removeListener("data", feed);
        if (!process.stdin.destroyed) process.stdin.destroy();
        // Exit 0 rather than re-raising a clean signal. Preserve a non-zero
        // status when the output transport itself failed.
        process.exitCode = outputFailed ? 1 : 0;
        // Leave the process through an explicit process.exit() rather than by
        // letting the event loop run dry. Keeping the signal listeners
        // registered (above) is not enough on its own: when Node exits because
        // the loop emptied, it first CLOSES its internal signal watchers and
        // only then finishes tearing the process down, and a signal delivered
        // inside that gap finds the default disposition again and kills the
        // process. A supervisor that sends a second, impatient SIGTERM a few
        // tens of milliseconds after the first lands squarely in that gap, so
        // the clean stop is reported as a signal death after all. 'beforeExit'
        // fires at the moment the loop would have drained — every in-flight
        // handler has finished and every stdout write has flushed, because
        // either would still be holding the loop open — and process.exit()
        // from there terminates immediately without the graceful handle
        // teardown, so the watchers stay armed until the process is gone.
        process.once("beforeExit", () => process.exit(process.exitCode ?? 0));
        // Installing a handler at all removes the default "die now", so this
        // timer is the backstop for a stuck handler or stdout.
        setTimeout(() => process.exit(process.exitCode ?? 0), SHUTDOWN_GRACE_MS).unref();
    };
    // End of stdin is the normal SSH-channel shutdown path. Use the same
    // idempotent cleanup and grace timer as an explicit signal.
    process.stdin.on("close", shutdown);
    for (const signal of SHUTDOWN_SIGNALS) process.on(signal, shutdown);
    return { shutdown };
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
    const mode = process.argv[2];
    if (mode === "rpc") {
        runStdio();
    } else {
        process.stderr.write("usage: codeharbord rpc --stdio\n");
        process.exit(2);
    }
}
