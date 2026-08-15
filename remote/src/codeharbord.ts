// codeharbord (SPEC 10): the remote workspace service. Launched over SSH as
// `codeharbord rpc --stdio` and speaks newline-delimited JSON-RPC 2.0 on
// stdin/stdout. It need not run permanently.
//
// Scope: the transport itself — request framing, dispatch, backpressure and
// shutdown — plus the two introspection handlers (`ping`, `server.info`) that
// belong to no method group. The `file.*`, `workspace.*` and `tmux.*` groups
// (SPEC 10.2) are implemented in files.ts, workspace.ts and tmux.ts and spread
// into the method table below.

// pathToFileURL, not string concatenation: process.argv[1] is a filesystem
// path, and a path containing a space (or any character a URL must
// percent-encode) would never compare equal to import.meta.url, silently
// turning the CLI entry point below into a no-op.
import { pathToFileURL } from "node:url";
import { homedir } from "node:os";
import { isAbsolute, join as pathJoin } from "node:path";

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
// closeDefaultWorkspace() releases that shared connection on an orderly stop;
// see the 'beforeExit' handler in runStdio for why it is called only there.
import {
    closeDefaultWorkspace,
    isDatabaseBusy,
    serverIdentity,
    WORKSPACE_METHODS,
} from "./workspace.ts";
// tmux session discovery (SPEC 10.2). Its own `tmux.*` method group, likewise
// outside the frozen C1 file catalog.
import { TMUX_METHODS } from "./tmux.ts";
// isInvalidParams recognizes the tagged error the param guards throw, so a
// malformed request is answered with "Invalid params" instead of a generic
// internal error that blames the server for the client's bad payload.
import {
    InvalidParamsError,
    isInvalidParams,
    requireBoolean,
    requireObject,
    requireString,
} from "./validate.ts";
// isPlainObject lives with the event schema (events.ts) because that is where
// the JSON-object footgun it guards against was first written down; it is the
// same check the bridge and the control listener apply to a decoded payload.
import { isPlainObject } from "./events.ts";
// Viewer control channel (agent -> desktop). Its socket is bound by runStdio
// below; the `viewer.commandResult` handler in the static method table settles
// the producer that is still waiting on it.
import { startControlListener, type ControlListener } from "./control.ts";
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
    RPC_VIEWER_COMMAND_NOTIFICATION,
    RPC_VIEWER_COMMAND_RESULT_METHOD,
    RPC_WATCH_EVENT_NOTIFICATION,
    RPC_WATCH_EVENTS_LOST_NOTIFICATION,
} from "./rpc-types.ts";
import type {
    PingResult,
    ServerInfoResult,
    ViewerCommandParams,
    ViewerCommandResultResult,
    WatchEvent,
    WatchEventsLost,
} from "./rpc-types.ts";
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
    RPC_VIEWER_COMMAND_NOTIFICATION,
    RPC_VIEWER_COMMAND_RESULT_METHOD,
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
export const RPC_SERVER_VERSION = "0.1.29";
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
// Bumped 6 -> 7 when the viewer control channel joined the transport
// (viewer.command / viewer.commandResult): a v6 server binds no control socket,
// so an agent's pane command would be accepted by its own tooling and then
// vanish with nothing anywhere saying the server cannot carry it.
export const RPC_SCHEMA_VERSION = 7;

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

// Longest string request id responseLine will echo back inside a refusal it had
// to synthesize. A kilobyte is orders of magnitude beyond every id the C++
// client mints (a decimal counter) while keeping the refusal comfortably inside
// one frame.
const MAX_REFUSAL_ID_BYTES = 1024;

type MethodHandler = (params: unknown) => unknown | Promise<unknown>;

/**
 * The remote user's XDG data directory, which server.info's `recoveryDir` is
 * built under.
 *
 * A RELATIVE $XDG_DATA_HOME is ignored rather than honoured, the same rule
 * resolveSocketPath applies to $XDG_RUNTIME_DIR (events.ts) and for the same
 * reason: the variable is DEFINED to be an absolute path, and joining a
 * relative one produces a recoveryDir that depends on this process's working
 * directory. The client stores that string, appends its per-pane id and sends
 * it back as a file.writeFile path, and files.ts passes a relative path
 * straight to the fs — so the snapshots would land under whatever directory the
 * daemon happened to be launched in, and a daemon started from anywhere else
 * would look for them somewhere different. Silently no crash recovery, which is
 * the one feature whose failure is only discovered after a crash.
 *
 * Read per request, not captured at import time, so a value exported after this
 * module was loaded is honoured (the same choice execFileRunner makes for
 * $CODEHARBOR_TMUX in tmux.ts).
 */
function recoveryBaseDir(): string {
    const dataHome = process.env.XDG_DATA_HOME;
    if (dataHome !== undefined && isAbsolute(dataHome)) return dataHome;
    return pathJoin(homedir(), ".local", "share");
}

// The live viewer control listener, or null when none is bound — a hand-started
// daemon that lost the socket race, or one whose stdout has already failed.
//
// Module-level rather than a runStdio() local because the `viewer.commandResult`
// handler below lives in the module's static method table, exactly as
// fileWatchService is a module singleton for the same reason. runStdio() owns
// its lifetime and clears it on shutdown, so a retired transport's listener can
// never settle a live one's producers.
let activeControlListener: ControlListener | null = null;

// Static method table. Anything outside the three spread-in method groups is
// declared here; a new group is added to the spread rather than to this literal.
const methods: Record<string, MethodHandler> = {
    // Transport keepalive (RPC_PING_METHOD). Deliberately the cheapest possible
    // handler: the C++ client's heartbeat drops the transport when several
    // consecutive probes go unanswered, so this must never touch the filesystem
    // or the database — an answer is meant to prove only that this process is
    // still reading its input and writing its output.
    [RPC_PING_METHOD]: (): PingResult => ({ pong: true }),
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
    [RPC_SERVER_INFO_METHOD]: (): ServerInfoResult => ({
        name: RPC_SERVER_NAME,
        version: RPC_SERVER_VERSION,
        schemaVersion: RPC_SCHEMA_VERSION,
        serverId: serverIdentity(),
        recoveryDir: pathJoin(recoveryBaseDir(), "codeharbor", "recovery"),
    }),
    // The desktop's answer to one relayed viewer command (SPEC 4.3). The client
    // sends it as an ordinary request because the transport carries requests in
    // that direction already; the listener matches it to the agent's still-open
    // control socket and writes the reply there.
    //
    // An unknown commandId returns SUCCESS on purpose: the daemon may already
    // have timed the command out, and an error here would surface in the client
    // as a failure toast for work that merely finished late. `error` and `data`
    // are forwarded verbatim — the client, not this relay, is the authority on
    // why a pane command was refused.
    [RPC_VIEWER_COMMAND_RESULT_METHOD]: (params: unknown): ViewerCommandResultResult => {
        const p = requireObject(params, RPC_VIEWER_COMMAND_RESULT_METHOD);
        const commandId = requireString(p, "commandId", RPC_VIEWER_COMMAND_RESULT_METHOD);
        const ok = requireBoolean(p, "ok", RPC_VIEWER_COMMAND_RESULT_METHOD);
        let error: { code: string; message: string } | undefined;
        if (p.error !== undefined) {
            const raw = requireObject(p.error, `${RPC_VIEWER_COMMAND_RESULT_METHOD}.error`);
            error = {
                code: requireString(raw, "code", `${RPC_VIEWER_COMMAND_RESULT_METHOD}.error`),
                message: requireString(raw, "message", `${RPC_VIEWER_COMMAND_RESULT_METHOD}.error`),
            };
        }
        let data: Record<string, unknown> | undefined;
        if (p.data !== undefined) {
            if (!isPlainObject(p.data)) {
                throw new InvalidParamsError(
                    `${RPC_VIEWER_COMMAND_RESULT_METHOD}: missing or invalid field 'data'`,
                );
            }
            data = p.data;
        }
        activeControlListener?.settle({ commandId, ok, error, data });
        return { ok: true };
    },
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
    // Object.hasOwn, not a bare index: `methods` is an object LITERAL, so it
    // inherits Object.prototype and a request naming an inherited member finds
    // a "handler" that was never registered. `method: "constructor"` is the
    // worst of them — Object(params) returns the params, so the request is
    // answered with a bogus SUCCESS echoing its own arguments instead of
    // "Method not found"; `method: "toString"` answers "[object Undefined]".
    // The rest ("valueOf", "hasOwnProperty", ...) throw a TypeError from the
    // call below and are reported as -32603, blaming the server for a method
    // the client made up. All of them must be -32601.
    const handler = Object.hasOwn(methods, value.method)
        ? methods[value.method]
        : undefined;
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
        // A valid request whose answer, or whose payload, would blow a server
        // bound: a directory listing too big for one transport frame, one watch
        // too many, or a workspace.setLayout split tree past
        // MAX_LAYOUT_TREE_BYTES (workspace.ts). Not -32603 for the same reason
        // as the busy code below: nothing malfunctioned and nothing was
        // half-applied. The message already names the limit and what to do, and
        // the client shows it verbatim.
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
 * Serialize one response into the single wire line that carries it.
 *
 * Two things can go wrong between a handler's return value and the wire, and
 * both must still produce SOME response: the client correlates replies by
 * request id, so a request answered with nothing leaves a pending call hanging
 * until the heartbeat gives up on the whole transport.
 *
 *   * The payload is not JSON-serializable (a reference cycle, or a BigInt).
 *     Answer with an internal error naming that.
 *   * The serialized payload is past MAX_LINE_BYTES. BOTH ends of this protocol
 *     drop the transport on an over-cap frame, so writing it anyway costs the
 *     user the entire workspace connection — every terminal and every editor on
 *     it — over one oversized reply, with nothing on screen saying why. Answer
 *     with RPC_RESOURCE_LIMIT instead, exactly as the handlers that DO know
 *     their own size (file.readFile, file.listDirectory) already do: the one
 *     request is refused, the message names the limit, and the session lives.
 *
 * WHAT THE CAP MEASURES: the JSON payload, EXCLUDING the newline that frames
 * it. That is the convention the inbound framer below already uses (it counts
 * only the bytes before the newline) and the one the C++ client uses for its
 * matching kMaxLineBytes. Measuring the framed line here instead — payload plus
 * newline — made the two directions disagree by exactly one byte: a payload of
 * precisely MAX_LINE_BYTES was accepted on the way in and refused on the way
 * out, while both comments claimed to state the same rule.
 *
 * THE REFUSAL MUST ITSELF FIT. Its message is a fixed string, but the id is
 * echoed from the request, and a client is free to send a string id as long as
 * the frame allows — so a refusal that copied a 16 MiB id would be over the cap
 * for exactly the reason it exists. An id past MAX_REFUSAL_ID_BYTES is therefore
 * replaced with null: that call cannot be correlated, but it is the one case
 * where the peer chose an id it could not be answered with, and the alternative
 * is dropping the whole transport.
 */
export function responseLine(response: RpcResponse): string {
    let refusal: { code: number; message: string };
    try {
        const payload = JSON.stringify(response);
        if (Buffer.byteLength(payload) <= MAX_LINE_BYTES) return `${payload}\n`;
        refusal = {
            code: RPC_RESOURCE_LIMIT,
            message:
                `The answer to this request is larger than the ${MAX_LINE_BYTES}-byte ` +
                "transport frame limit, so it was refused. Nothing was changed.",
        };
    } catch {
        refusal = { code: RPC_INTERNAL_ERROR, message: "Response could not be serialized" };
    }
    const refusalId =
        typeof response.id === "string" &&
        Buffer.byteLength(response.id) > MAX_REFUSAL_ID_BYTES
            ? null
            : response.id;
    return `${JSON.stringify({
        jsonrpc: "2.0",
        id: refusalId,
        error: refusal,
    })}\n`;
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
    // Defaults to the RPC transport's own cap, which is what every existing
    // caller wants. Parameterized because the MCP server (remote/src/mcp) frames
    // the same way over a DIFFERENT protocol whose messages are three orders of
    // magnitude smaller — reusing this framer there is what keeps the two stdio
    // readers from disagreeing about what a line is, and a 16 MiB bound on a
    // protocol that never sends more than a few kilobytes is not a bound.
    maxLineBytes: number = MAX_LINE_BYTES,
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
                if (lineBytes > maxLineBytes) {
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
        if (heldBytes > maxLineBytes) {
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
        // The write is GUARDED, exactly like runStdio's writeOut: a stream whose
        // far end has gone (a destroyed or already-ended stdout) can raise
        // synchronously, and this function is reached from the filesystem watch
        // callback in files.ts. An escaping exception there is not caught by
        // anything — it is an uncaught exception that kills the whole daemon,
        // taking every terminal and editor of the session with it, over a
        // notification that was already undeliverable. Treat a throwing output
        // as stalled so subsequent notifications queue (and are reported lost
        // past the bound) instead of throwing again per event; stdout's 'error'
        // event is what drives the actual transport teardown.
        let accepted: boolean;
        try {
            accepted = out.write(line);
        } catch {
            accepted = false;
        }
        // A false return means Node is now buffering internally; everything
        // after it must queue instead of adding to that buffer.
        if (!accepted) {
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
        const params: WatchEventsLost = { subscriptionIds };
        writeLine(
            `${JSON.stringify({
                jsonrpc: "2.0",
                method: RPC_WATCH_EVENTS_LOST_NOTIFICATION,
                params,
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
    // Each queued entry carries its own byte length. Recomputing it on flush
    // rescans a response line that may be megabytes long, for a number that was
    // already measured when the line was queued.
    const pendingResponses: { line: string; bytes: number }[] = [];
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
    // KEEP the unsubscribe functions the watch service hands back. runStdio owns
    // process-global state (the fileWatchService singleton, process.stdout), and
    // `shutdown` is deliberately exposed so a caller can stop this transport
    // without signalling the process — which means a second runStdio() in the
    // same process is a supported shape and must not inherit the first one's
    // listeners. Discarding these left the retired relay subscribed: one watch
    // event would then be written to stdout once per retired transport, i.e.
    // duplicate notifications, and the retired failOutput would answer a stdout
    // error by calling closeAll() on the LIVE transport's subscriptions.
    const releaseWatchEvents = fileWatchService.onWatchEvent((event) =>
        watchRelay.deliver(event),
    );
    // No queue outlives its subscriber: unwatch, and closeAll on stdin close,
    // both announce the release here.
    const releaseWatchClosed = fileWatchService.onWatchClosed((id) =>
        watchRelay.forget(id),
    );
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
        // outputStalled is cleared by the watch relay's own 'drain' listener,
        // which runs FIRST (it was registered first) and may re-stall the stream
        // flushing its queued notifications. Without this guard the flush below
        // would write into a stream that is buffering internally again, past
        // the bound this queue exists to enforce. The next 'drain' gets it.
        if (outputFailed || outputStalled) return;
        while (pendingResponseIndex < pendingResponses.length) {
            const entry = pendingResponses[pendingResponseIndex++];
            pendingResponseBytes -= entry.bytes;
            try {
                if (!process.stdout.write(entry.line)) {
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
        } else {
            // Drop the already-written prefix. The index alone keeps the queue
            // O(1) to advance, but the array goes on REFERENCING every response
            // it has handed to stdout, so a stream that stalls, drains a little
            // and stalls again holds megabytes of delivered lines that no longer
            // count against pendingResponseBytes — the bound stops describing
            // the memory actually held.
            pendingResponses.splice(0, pendingResponseIndex);
            pendingResponseIndex = 0;
        }
    };
    process.stdout.on("drain", flushResponses);
    process.stdout.on("error", failOutput);
    const writeOut = (line: string): void => {
        if (outputFailed) return;
        const bytes = Buffer.byteLength(line);
        // Backstop only: responseLine() has already replaced an over-cap answer
        // with a small refusal, so nothing should reach here. A frame past the
        // cap makes the peer drop the whole transport, so fail rather than write
        // a line that is known to be doomed.
        //
        // `line` always ends with the framing newline, and the cap measures the
        // PAYLOAD without it (see responseLine). Comparing the framed length
        // here would have made this backstop one byte stricter than the rule
        // responseLine enforces, so an answer of exactly MAX_LINE_BYTES — which
        // responseLine deliberately passes — would have torn the transport down
        // instead of being delivered.
        if (bytes - 1 > MAX_LINE_BYTES) {
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
            pendingResponses.push({ line, bytes });
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
    // VIEWER CONTROL (SPEC 4.3). Bind the agent-facing socket and relay each
    // command it accepts as an id-less notification on this transport.
    //
    // The relay goes through writeOut, NOT process.stdout directly: that is the
    // bounded response path, and a notification written past it would grow
    // Node's internal writable buffer without limit whenever the SSH channel
    // stalls. Its return of `!outputFailed` is what turns a dead output into an
    // immediate `failed` for the waiting agent instead of a five-second wait for
    // a reply that can never arrive.
    //
    // Binding is ASYNCHRONOUS and its failure is a stderr line, never a fault: a
    // second CodeHarbor window against the same host loses the socket race, and
    // SshChannelDevice publishes daemon stderr as diagnostics (SessionBootstrap
    // demotes it deliberately), so that window degrades to "no agent pane
    // control" instead of failing to connect at all.
    const relayViewerCommand = (command: {
        commandId: string;
        devSessionId: string;
        terminalId: string;
        op: string;
        args: Record<string, unknown>;
    }): boolean => {
        if (outputFailed) return false;
        const params: ViewerCommandParams = {
            commandId: command.commandId,
            devSessionId: command.devSessionId,
            terminalId: command.terminalId,
            op: command.op,
            args: command.args,
        };
        writeOut(
            `${JSON.stringify({
                jsonrpc: "2.0",
                method: RPC_VIEWER_COMMAND_NOTIFICATION,
                params,
            })}\n`,
        );
        return !outputFailed;
    };
    let controlListener: ControlListener | null = null;
    void startControlListener(relayViewerCommand).then(
        (listener) => {
            // A shutdown that ran while the bind was still in flight must not
            // leave a live socket behind.
            if (shuttingDown) {
                listener.close();
                return;
            }
            controlListener = listener;
            activeControlListener = listener;
            process.stderr.write(
                `codeharbord: viewer control listening on ${listener.socketPath}\n`,
            );
        },
        (err: unknown) => {
            process.stderr.write(
                `codeharbord: viewer control disabled: ${err instanceof Error ? err.message : String(err)}\n`,
            );
        },
    );
    const dispatchLine = (line: string): void => {
        // Skip separator lines without copying the line: trim() on a multi-MiB
        // base64 file.writeFile frame duplicates the whole string just to answer
        // "is this blank?". The C++ reader avoids the same copy deliberately.
        if (!/\S/.test(line)) return;
        // `.then(onFulfilled, onRejected)`, NOT `.then(...).catch(...)`: a
        // catch chained AFTER the success handler also catches anything the
        // success handler itself throws, so a failure inside writeOut would
        // answer the SAME request a second time — an extra id-less error line
        // the client cannot correlate, on top of the reply it already got.
        // The two-argument form keeps the rejection handler responsible for
        // the request's rejection only.
        void handleLine(line).then(
            (response) => {
                if (response) writeOut(responseLine(response));
            },
            () => {
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
            },
        );
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
        // Detach this transport's watch relay, AFTER closeAll() so the relay is
        // still subscribed to hear each release and drop the notifications it
        // was holding for it.
        releaseWatchEvents();
        releaseWatchClosed();
        // Stop accepting viewer commands and answer every agent still waiting on
        // one, BEFORE the responses below are flushed: those producers are
        // blocking an agent's turn, and a socket closed without a reply leaves
        // that agent waiting for its own client-side timeout instead of being
        // told the session is going away. Clearing the module-level handle keeps
        // a retired transport from settling a later one's producers.
        controlListener?.close();
        if (activeControlListener === controlListener) activeControlListener = null;
        // Stop reading requests. Answering a new one while shutting down would
        // start work whose reply cannot be guaranteed to reach the wire.
        process.stdin.removeListener("data", feed);
        if (!process.stdin.destroyed) process.stdin.destroy();
        // Hand every queued response to stdout unconditionally. They are queued
        // only because the PEER stalled, and Node keeps the process alive until
        // its writable buffer flushes, so this is the last chance for a reply
        // the client is still waiting on — dropping them left those calls
        // hanging until the client's heartbeat tore the transport down. The
        // queue is bounded by MAX_PENDING_RPC_RESPONSE_BYTES so this cannot
        // buffer without limit, and the grace timer below is the backstop for a
        // peer that never reads again.
        if (!outputFailed) {
            for (let i = pendingResponseIndex; i < pendingResponses.length; i += 1) {
                try {
                    process.stdout.write(pendingResponses[i].line);
                } catch {
                    break;
                }
            }
        }
        pendingResponses.length = 0;
        pendingResponseIndex = 0;
        pendingResponseBytes = 0;
        // Detach the stdout listeners this transport installed. The queue above
        // is now empty and flushed by hand, so there is nothing left for a
        // 'drain' to do, and a retired failOutput answering a later stdout error
        // would call closeAll() on whatever transport is live by then. The two
        // no-op 'error' swallowers stay: an EPIPE from the flush just above, or
        // from Node draining its writable buffer after this returns, must not
        // become an uncaught exception, and a no-op listener can do no harm.
        process.stdout.removeListener("drain", flushResponses);
        process.stdout.removeListener("error", failOutput);
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
        process.once("beforeExit", () => {
            // The ONLY safe point to close the workspace database. The loop has
            // drained here, so no workspace.* handler can still be holding a
            // statement open — closing while one is mid-query would turn a
            // reply that was about to succeed into "database is not open".
            //
            // Skipping the close is not a durability problem: SQLite's
            // write-ahead log is crash-safe by construction, which it has to be
            // because a daemon can be SIGKILLed or lose its machine at any
            // moment. What the close buys is an orderly stop that CHECKPOINTS
            // the log and removes the -wal/-shm files, instead of leaving a
            // write-ahead log behind for the next process to recover on open.
            // That is also why the grace timer below deliberately does NOT call
            // it: that path exists for a handler that is stuck, i.e. exactly the
            // case where a statement may still be running.
            closeDefaultWorkspace();
            process.exit(process.exitCode ?? 0);
        });
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
    const [mode, ...flags] = process.argv.slice(2);
    // The documented invocation is `codeharbord rpc --stdio` (SPEC 10.1), and
    // stdio is the only transport there has ever been, so the flag stays
    // OPTIONAL for compatibility with any launcher that omits it. What is not
    // acceptable is accepting an argument that asks for something else:
    // `codeharbord rpc --http` used to start the stdio server anyway and then
    // sit there reading a stdin nobody was writing to, which looks exactly like
    // a hung daemon. Anything that is not the stdio flag is a usage error.
    const unknown = flags.find((flag) => flag !== "--stdio");
    if (mode === "rpc" && unknown === undefined) {
        runStdio();
    } else {
        if (mode === "rpc") {
            process.stderr.write(`codeharbord: unknown option '${unknown}'\n`);
        }
        process.stderr.write("usage: codeharbord rpc --stdio\n");
        process.exit(2);
    }
}
