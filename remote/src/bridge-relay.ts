// Internal reader transport for the per-user agent-event relay. Producers keep
// the existing socket address and event envelopes; readers register separately.
import net from "node:net";
import { spawn } from "node:child_process";
import { setTimeout as delay } from "node:timers/promises";

export const BRIDGE_SUBSCRIBE_PROTOCOL = "codeharbor-bridge-subscribe";
export const BRIDGE_SUBSCRIBE_ACK = "codeharbor-bridge-subscribed";
export const BRIDGE_SUBSCRIBE_VERSION = 1;
export const MAX_BRIDGE_SUBSCRIBE_BYTES = 1024;
export const BRIDGE_RELAY_IDLE_MS = 30_000;
export const BRIDGE_RELAY_IDLE_ENV = "CODEHARBOR_BRIDGE_RELAY_IDLE_MS";
export const MAX_BRIDGE_READERS = 64;
export const MAX_BRIDGE_READER_BYTES = 2 * 1024 * 1024;
export const MAX_BRIDGE_READER_MESSAGES = 128;
export const MAX_BRIDGE_RELAY_BYTES = 16 * 1024 * 1024;
export const MAX_BRIDGE_RELAY_MESSAGES = 1024;

export interface BridgeSubscriberOptions {
    idleTimeoutMs?: number;
    maxReaders?: number;
    maxReaderBytes?: number;
    maxReaderMessages?: number;
    maxTotalBytes?: number;
    maxTotalMessages?: number;
}

export function bridgeRelayIdleMs(env: NodeJS.ProcessEnv = process.env): number {
    const value = env[BRIDGE_RELAY_IDLE_ENV];
    if (value === undefined) return BRIDGE_RELAY_IDLE_MS;
    const parsed = Number(value);
    return Number.isSafeInteger(parsed) && parsed > 0 && parsed <= BRIDGE_RELAY_IDLE_MS
        ? parsed : BRIDGE_RELAY_IDLE_MS;
}

export function parseBridgeSubscription(line: string): { version: unknown } | null {
    if (Buffer.byteLength(line) > MAX_BRIDGE_SUBSCRIBE_BYTES) return null;
    let value: unknown;
    try {
        value = JSON.parse(line);
    } catch {
        return null;
    }
    if (typeof value !== "object" || value === null || Array.isArray(value)) return null;
    const request = value as Record<string, unknown>;
    if (request.protocol !== BRIDGE_SUBSCRIBE_PROTOCOL || Object.keys(request).length !== 2 ||
        !Object.hasOwn(request, "version")) return null;
    return { version: request.version };
}

interface Reader {
    socket: net.Socket;
    queue: { line: string; bytes: number }[];
    bytes: number;
    messages: number;
    writing: boolean;
    closed: boolean;
}

/** Queue budgets include the one write currently owned by the socket. */
export class BridgeSubscribers {
    private readonly readers = new Map<net.Socket, Reader>();
    private readonly options: Required<BridgeSubscriberOptions>;
    private readonly onIdle: () => void;
    private idle: NodeJS.Timeout | undefined;
    private bytes = 0;
    private messages = 0;
    private closed = false;

    constructor(onIdle: () => void, options: BridgeSubscriberOptions = {}) {
        this.onIdle = onIdle;
        this.options = {
            idleTimeoutMs: options.idleTimeoutMs ?? BRIDGE_RELAY_IDLE_MS,
            maxReaders: options.maxReaders ?? MAX_BRIDGE_READERS,
            maxReaderBytes: options.maxReaderBytes ?? MAX_BRIDGE_READER_BYTES,
            maxReaderMessages: options.maxReaderMessages ?? MAX_BRIDGE_READER_MESSAGES,
            maxTotalBytes: options.maxTotalBytes ?? MAX_BRIDGE_RELAY_BYTES,
            maxTotalMessages: options.maxTotalMessages ?? MAX_BRIDGE_RELAY_MESSAGES,
        };
        this.armIdle();
    }

    private armIdle(): void {
        if (this.closed || this.readers.size !== 0 || this.idle !== undefined) return;
        // Producer traffic deliberately does not extend readerless lifetime.
        this.idle = setTimeout(() => {
            this.idle = undefined;
            if (!this.closed && this.readers.size === 0) this.onIdle();
        }, this.options.idleTimeoutMs);
    }

    private retire(reader: Reader): void {
        if (reader.closed) return;
        reader.closed = true;
        this.readers.delete(reader.socket);
        this.bytes -= reader.bytes;
        this.messages -= reader.messages;
        reader.bytes = 0;
        reader.messages = 0;
        reader.queue.length = 0;
        reader.socket.destroy();
        this.armIdle();
    }

    private pump(reader: Reader): void {
        if (reader.closed || reader.writing) return;
        const entry = reader.queue.shift();
        if (!entry) return;
        reader.writing = true;
        try {
            // One write at a time: neither a false write result nor a delayed
            // drain can move uncounted bytes into Node's writable queue.
            reader.socket.write(entry.line, (err?: Error | null) => {
                if (reader.closed) return;
                if (err) {
                    this.retire(reader);
                    return;
                }
                reader.writing = false;
                reader.bytes -= entry.bytes;
                reader.messages -= 1;
                this.bytes -= entry.bytes;
                this.messages -= 1;
                this.pump(reader);
            });
        } catch {
            this.retire(reader);
        }
    }

    private enqueue(reader: Reader, line: string, bytes: number): void {
        if (reader.closed) return;
        if (reader.bytes + bytes > this.options.maxReaderBytes ||
            reader.messages + 1 > this.options.maxReaderMessages ||
            bytes > this.options.maxTotalBytes) {
            this.retire(reader);
            return;
        }
        while (this.bytes + bytes > this.options.maxTotalBytes ||
            this.messages + 1 > this.options.maxTotalMessages) {
            // Retire backlog owners under global pressure, not whichever fast
            // reader happens to receive the next event after a slow one.
            const countLimited = this.messages + 1 > this.options.maxTotalMessages;
            let slowest: Reader | undefined;
            for (const candidate of this.readers.values()) {
                if (candidate.messages === 0) continue;
                if (!slowest || (countLimited
                    ? candidate.messages > slowest.messages
                    : candidate.bytes > slowest.bytes)) slowest = candidate;
            }
            if (!slowest) {
                this.retire(reader);
                return;
            }
            this.retire(slowest);
            if (reader.closed) return;
        }
        reader.bytes += bytes;
        reader.messages += 1;
        this.bytes += bytes;
        this.messages += 1;
        reader.queue.push({ line, bytes });
        this.pump(reader);
    }

    subscribe(socket: net.Socket, version: unknown): boolean {
        if (version !== BRIDGE_SUBSCRIBE_VERSION) {
            socket.end(`${JSON.stringify({
                protocol: BRIDGE_SUBSCRIBE_ACK, version: BRIDGE_SUBSCRIBE_VERSION,
                ok: false, error: "unsupported agent-event subscription version",
            })}\n`, () => socket.destroy());
            return false;
        }
        return this.add(socket);
    }

    add(socket: net.Socket): boolean {
        if (this.closed || socket.destroyed || this.readers.size >= this.options.maxReaders) {
            socket.destroy();
            return false;
        }
        if (this.readers.has(socket)) return false;
        if (this.idle !== undefined) {
            clearTimeout(this.idle);
            this.idle = undefined;
        }
        const reader: Reader = { socket, queue: [], bytes: 0, messages: 0, writing: false, closed: false };
        this.readers.set(socket, reader);
        socket.on("error", () => this.retire(reader));
        socket.once("close", () => this.retire(reader));
        const ack = `${JSON.stringify({
            protocol: BRIDGE_SUBSCRIBE_ACK, version: BRIDGE_SUBSCRIBE_VERSION, ok: true,
        })}\n`;
        this.enqueue(reader, ack, Buffer.byteLength(ack));
        return !reader.closed;
    }

    broadcast(line: string): void {
        if (this.closed) return;
        const bytes = Buffer.byteLength(line);
        for (const reader of this.readers.values()) this.enqueue(reader, line, bytes);
    }

    readerCount(): number { return this.readers.size; }
    pendingBytes(): number { return this.bytes; }
    pendingMessages(): number { return this.messages; }

    close(): void {
        if (this.closed) return;
        this.closed = true;
        clearTimeout(this.idle);
        this.idle = undefined;
        for (const reader of this.readers.values()) this.retire(reader);
    }
}

export class BridgeSubscriptionError extends Error {
    readonly kind: "absent" | "incompatible" | "transport";
    constructor(message: string, kind: "absent" | "incompatible" | "transport") {
        super(message);
        this.name = "BridgeSubscriptionError";
        this.kind = kind;
    }
}

export interface BridgeConnectOptions {
    handshakeTimeoutMs?: number;
    signal?: AbortSignal;
}

/** Resolves paused, with the acknowledgement consumed and event bytes retained. */
export function connectBridgeSubscriber(
    socketPath: string,
    options: BridgeConnectOptions = {},
): Promise<net.Socket> {
    const { promise, resolve, reject } = Promise.withResolvers<net.Socket>();
    const socket = new net.Socket();
    // Keep late destroy/write errors handled across the promise handoff.
    socket.on("error", () => {});
    let connected = false;
    let settled = false;
    let held: Buffer[] = [];
    let heldBytes = 0;
    const abort = (): void => finish(new Error("agent-event subscription cancelled"));
    const timer = setTimeout(() => finish(new BridgeSubscriptionError(
        connected
            ? `the listener at ${socketPath} does not acknowledge shared agent-event subscriptions; upgrade and restart the existing bridge`
            : `timed out connecting to the agent-event relay at ${socketPath}`,
        connected ? "incompatible" : "transport",
    )), options.handshakeTimeoutMs ?? 1000);
    const onError = (err: NodeJS.ErrnoException): void => finish(new BridgeSubscriptionError(
        `cannot subscribe to the agent-event relay at ${socketPath}: ${err.message}`,
        !connected && (err.code === "ENOENT" || err.code === "ECONNREFUSED") ? "absent" : "transport",
    ));
    const onClose = (): void => finish(new BridgeSubscriptionError(
        `the listener at ${socketPath} closed without acknowledging shared subscriptions; upgrade and restart the existing bridge`,
        "incompatible",
    ));
    const cleanup = (): void => {
        clearTimeout(timer);
        options.signal?.removeEventListener("abort", abort);
        socket.removeListener("error", onError);
        socket.removeListener("close", onClose);
        socket.removeListener("data", onData);
    };
    const finish = (err?: Error): void => {
        if (settled) return;
        settled = true;
        cleanup();
        held = [];
        if (err) {
            socket.destroy();
            reject(err);
        } else {
            resolve(socket);
        }
    };
    const onData = (chunk: Buffer): void => {
        const newline = chunk.indexOf(0x0a);
        const prefix = newline < 0 ? chunk : chunk.subarray(0, newline);
        heldBytes += prefix.length;
        if (heldBytes > MAX_BRIDGE_SUBSCRIBE_BYTES) {
            finish(new BridgeSubscriptionError("the agent-event relay sent an oversized subscription acknowledgement", "incompatible"));
            return;
        }
        held.push(prefix);
        if (newline < 0) return;
        let ack: unknown;
        try {
            ack = JSON.parse(Buffer.concat(held, heldBytes).toString("utf8"));
        } catch {
            finish(new BridgeSubscriptionError("the existing bridge sent an invalid subscription acknowledgement; upgrade and restart it", "incompatible"));
            return;
        }
        if (typeof ack !== "object" || ack === null || Array.isArray(ack) ||
            (ack as Record<string, unknown>).protocol !== BRIDGE_SUBSCRIBE_ACK ||
            (ack as Record<string, unknown>).version !== BRIDGE_SUBSCRIBE_VERSION ||
            (ack as Record<string, unknown>).ok !== true) {
            finish(new BridgeSubscriptionError("the existing bridge does not support this subscription version; upgrade and restart it", "incompatible"));
            return;
        }
        socket.pause();
        const remaining = chunk.subarray(newline + 1);
        if (remaining.length > 0) socket.unshift(remaining);
        finish();
    };
    socket.on("error", onError);
    socket.once("close", onClose);
    socket.on("data", onData);
    socket.once("connect", () => {
        connected = true;
        try {
            socket.write(`${JSON.stringify({ protocol: BRIDGE_SUBSCRIBE_PROTOCOL, version: BRIDGE_SUBSCRIBE_VERSION })}\n`);
        } catch (err) {
            finish(err instanceof Error ? err : new Error(String(err)));
        }
    });
    options.signal?.addEventListener("abort", abort, { once: true });
    if (options.signal?.aborted) {
        abort();
        return promise;
    }
    try {
        socket.connect(socketPath);
    } catch (err) {
        finish(err instanceof Error ? err : new Error(String(err)));
    }
    return promise;
}

export interface BridgeProxyOptions {
    startupTimeoutMs?: number;
    handshakeTimeoutMs?: number;
    retryDelayMs?: number;
}

/** The native session owns this proxy; the detached relay owns no SSH handles. */
export async function runBridgeProxy(
    socketPath: string,
    scriptPath: string,
    out: NodeJS.WritableStream = process.stdout,
    diagnostics: NodeJS.WritableStream = process.stderr,
    options: BridgeProxyOptions = {},
): Promise<void> {
    const controller = new AbortController();
    const stopped = Promise.withResolvers<void>();
    let failure: Error | undefined;
    let socket: net.Socket | undefined;
    const stop = (err?: Error): void => {
        if (controller.signal.aborted) return;
        failure = err;
        controller.abort();
        socket?.destroy();
        stopped.resolve();
    };
    const onSignal = (): void => stop();
    const onOutputError = (err: Error): void => stop(err);
    const onOutputClose = (): void => stop();
    const onDrain = (): void => { if (!controller.signal.aborted) socket?.resume(); };
    const signals = ["SIGHUP", "SIGINT", "SIGTERM"] as const;
    for (const signal of signals) process.on(signal, onSignal);
    out.on("error", onOutputError);
    out.on("close", onOutputClose);
    out.on("drain", onDrain);
    try {
        const deadline = Date.now() + (options.startupTimeoutMs ?? 5000);
        let launched = false;
        let launchError: Error | undefined;
        for (;;) {
            try {
                socket = await connectBridgeSubscriber(socketPath, {
                    handshakeTimeoutMs: options.handshakeTimeoutMs,
                    signal: controller.signal,
                });
                break;
            } catch (err) {
                if (controller.signal.aborted) break;
                if (!(err instanceof BridgeSubscriptionError) || err.kind !== "absent") throw err;
                if (launchError) throw launchError;
                if (!launched) {
                    launched = true;
                    const relay = spawn(process.execPath, [scriptPath, "--relay"], {
                        detached: true,
                        stdio: "ignore",
                        env: process.env,
                    });
                    relay.once("error", (error) => { launchError = error; });
                    relay.unref();
                }
                if (Date.now() >= deadline) throw new Error(`the shared agent-event relay did not start at ${socketPath}`);
                await delay(options.retryDelayMs ?? 50, undefined, { signal: controller.signal });
            }
        }
        if (socket && !controller.signal.aborted) {
            socket.on("error", (err: Error) => stop(err));
            socket.once("close", () => stop(new Error("the shared agent-event relay disconnected")));
            socket.on("data", (chunk: Buffer) => {
                try {
                    if (!out.write(chunk)) socket?.pause();
                } catch (err) {
                    stop(err instanceof Error ? err : new Error(String(err)));
                }
            });
            diagnostics.write(`codeharbor-bridge listening on ${socketPath}\n`);
            socket.resume();
            await stopped.promise;
        }
    } catch (err) {
        if (!controller.signal.aborted) throw err;
    } finally {
        socket?.destroy();
        for (const signal of signals) process.removeListener(signal, onSignal);
        out.removeListener("error", onOutputError);
        out.removeListener("close", onOutputClose);
        out.removeListener("drain", onDrain);
        // A buffered native stdout write can still fail after proxy shutdown.
        if (out === process.stdout) out.on("error", () => {});
    }
    if (failure) throw failure;
}
