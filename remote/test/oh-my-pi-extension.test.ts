import { test } from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import fs from "node:fs";
import path from "node:path";
import ohMyPiExtension, {
    createForwarder,
    toHookInput,
    FORWARDED_EVENTS,
    type OhMyPiHookApi,
} from "../src/hooks/oh-my-pi-extension.ts";
import { processBridgeLine } from "../src/bridge.ts";
import type { AgentState } from "../src/events.ts";
import { startFakeBridge } from "./fake-bridge.ts";

/** Stands in for the harness's extension API and lets a test fire events. */
class FakePi implements OhMyPiHookApi {
    readonly registrations: string[] = [];
    private readonly handlers = new Map<string, ((event: unknown) => unknown)[]>();

    on(event: string, handler: (event: unknown) => unknown): void {
        this.registrations.push(event);
        const existing = this.handlers.get(event);
        if (existing) existing.push(handler);
        else this.handlers.set(event, [handler]);
    }

    /** Fire one event exactly as the harness would, and await what it returns. */
    async emit(event: string, payload: unknown = { type: event }): Promise<unknown[]> {
        const results: unknown[] = [];
        for (const handler of this.handlers.get(event) ?? []) {
            results.push(await handler(payload));
        }
        return results;
    }
}

/** The states a recorded run of lines maps to, dropping the no-op events. */
function statesOf(lines: readonly string[]): AgentState[] {
    const states: AgentState[] = [];
    for (const line of lines) {
        const event = processBridgeLine(line);
        if (event !== null) states.push(event.state);
    }
    return states;
}

test("the extension forwards every lifecycle event as a bridge message the relay maps", async () => {
    const bridge = await startFakeBridge();
    try {
        const pi = new FakePi();
        ohMyPiExtension(pi, {
            XDG_RUNTIME_DIR: bridge.dir,
            OMP_DEV_SESSION_ID: "sess-ext",
            OMP_TERMINAL_ID: "term-ext",
        } as NodeJS.ProcessEnv);

        // Exactly the documented set is registered — a handler on an event SPEC
        // 6.5 does not map would spend a socket connection to produce nothing.
        assert.deepEqual(
            [...new Set(pi.registrations)].sort(),
            [...FORWARDED_EVENTS].sort(),
        );

        await pi.emit("session_start");
        await pi.emit("agent_start");
        await pi.emit("tool_call", { type: "tool_call", toolName: "bash", toolCallId: "c1" });
        await pi.emit("tool_result", { type: "tool_result", toolName: "bash", isError: false });
        await pi.emit("agent_end", { type: "agent_end", willContinue: false, messages: [] });
        // The shutdown handler's flush is what the harness awaits; the whole
        // point of it is that everything queued is on the wire by the time it
        // resolves, so the test may assert immediately afterwards.
        await pi.emit("session_shutdown");
        await bridge.waitForLines(6);

        const messages = bridge.lines.map((line) => JSON.parse(line) as Record<string, unknown>);
        for (const message of messages) {
            assert.equal(message.version, 1);
            assert.equal(message.harness, "oh-my-pi");
            assert.equal(message.devSessionId, "sess-ext");
            assert.equal(message.terminalId, "term-ext");
        }
        // In the order they fired: the serialized queue is what guarantees this,
        // and an out-of-order arrival would hand the client's monitor a stale
        // state as its latest word.
        assert.deepEqual(
            messages.map((message) => (message.native as Record<string, unknown>).type),
            [
                "session_start",
                "agent_start",
                "tool_call",
                "tool_result",
                "agent_end",
                "session_shutdown",
            ],
        );
        // The harness spells it `toolName`; only `tool` goes on the wire.
        const toolCall = messages[2].native as Record<string, unknown>;
        assert.equal(toolCall.tool, "bash");
        assert.equal(toolCall.toolName, undefined);

        // What the sidebar actually runs on: a full run reports starting, work,
        // completion and shutdown. tool_result for a non-ask tool is a no-op.
        assert.deepEqual(statesOf(bridge.lines), [
            "starting",
            "running",
            "running",
            "idle_unseen",
            "stopped",
        ]);
    } finally {
        await bridge.close();
    }
});

test("a tool_call for the ask tool reaches waiting_input, and its result releases it", async () => {
    const bridge = await startFakeBridge();
    try {
        const forwarder = createForwarder(
            { OMP_DEV_SESSION_ID: "sess-ask", OMP_TERMINAL_ID: "term-ask" } as NodeJS.ProcessEnv,
            bridge.socketPath,
        );
        assert.notEqual(forwarder, null);
        // The whole "the agent is waiting for you" feature hangs off this one
        // translation: the harness says `toolName`, the mapping reads `tool`,
        // and without the producer normalizing between them waiting_input could
        // never be reached by any real run.
        forwarder?.forward("tool_call", { type: "tool_call", toolName: "ask", input: {} });
        forwarder?.forward("tool_result", { type: "tool_result", toolName: "ask", content: [] });
        await forwarder?.flush();
        await bridge.waitForLines(2);
        assert.deepEqual(statesOf(bridge.lines), ["waiting_input", "running"]);
    } finally {
        await bridge.close();
    }
});

test("an agent_end that says it will continue is not reported as a completion", async () => {
    const bridge = await startFakeBridge();
    try {
        const forwarder = createForwarder(
            { OMP_DEV_SESSION_ID: "sess-cont", OMP_TERMINAL_ID: "term-cont" } as NodeJS.ProcessEnv,
            bridge.socketPath,
        );
        forwarder?.forward("agent_end", { type: "agent_end", willContinue: true });
        forwarder?.forward("agent_end", { type: "agent_end", willContinue: false });
        forwarder?.forward("agent_end", { type: "agent_end" });
        await forwarder?.flush();
        await bridge.waitForLines(3);
        // willContinue survives the wire — the producer forwards the flag and
        // the bridge is the one deciding what it means.
        const first = JSON.parse(bridge.lines[0]) as { native: Record<string, unknown> };
        assert.equal(first.native.willContinue, true);
        // Announcing "Agent finished" mid-run also arms the unseen badge and
        // raises a desktop notification, so the false completion is not a
        // cosmetic wrong word.
        assert.deepEqual(statesOf(bridge.lines), ["running", "idle_unseen", "idle_unseen"]);
    } finally {
        await bridge.close();
    }
});

test("an agent outside a CodeHarbor pane produces no traffic and no output", async () => {
    const bridge = await startFakeBridge();
    const realWrite = process.stderr.write.bind(process.stderr);
    const written: string[] = [];
    try {
        // Neither coordinate: the ordinary case for `omp` run in a plain shell.
        assert.equal(
            createForwarder({ XDG_RUNTIME_DIR: bridge.dir } as NodeJS.ProcessEnv),
            null,
        );
        // A blank coordinate is the same case: an event stamped with it is
        // structurally valid all the way to the client, which files it under a
        // Dev Session that does not exist.
        assert.equal(
            createForwarder({
                XDG_RUNTIME_DIR: bridge.dir,
                OMP_DEV_SESSION_ID: "sess-1",
                OMP_TERMINAL_ID: "   ",
            } as NodeJS.ProcessEnv),
            null,
        );

        const pi = new FakePi();
        process.stderr.write = ((chunk: string | Uint8Array): boolean => {
            written.push(typeof chunk === "string" ? chunk : Buffer.from(chunk).toString("utf8"));
            return true;
        }) as typeof process.stderr.write;
        ohMyPiExtension(pi, { XDG_RUNTIME_DIR: bridge.dir } as NodeJS.ProcessEnv);
        // Nothing registered at all, so a run outside CodeHarbor does not pay
        // even a per-event check — and cannot print a word on every tool call.
        assert.deepEqual(pi.registrations, []);
    } finally {
        process.stderr.write = realWrite;
        await bridge.close();
    }
    assert.deepEqual(written, []);
    assert.equal(bridge.connections, 0);
    assert.deepEqual(bridge.lines, []);
});

test("a dead bridge socket never throws into the agent (SPEC 6.4)", async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ch-omp-dead-"));
    const rejections: unknown[] = [];
    const onRejection = (reason: unknown): void => {
        rejections.push(reason);
    };
    process.on("unhandledRejection", onRejection);
    try {
        const pi = new FakePi();
        // A runtime directory with no socket in it: CodeHarbor is not running,
        // or its bridge died while the agent kept working.
        ohMyPiExtension(pi, {
            XDG_RUNTIME_DIR: dir,
            OMP_DEV_SESSION_ID: "sess-dead",
            OMP_TERMINAL_ID: "term-dead",
        } as NodeJS.ProcessEnv);
        // A throw out of a tool_call handler is not swallowed by the harness —
        // it BLOCKS the tool call — so a dead socket must not be able to stop
        // the agent from running tools.
        const results = await pi.emit("tool_call", { type: "tool_call", toolName: "bash" });
        assert.deepEqual(results, [undefined]);
        // The shutdown flush reports the same way: by resolving.
        await assert.doesNotReject(pi.emit("session_shutdown"));
        // An unhandled rejection inside the agent's process is itself the
        // "broken CodeHarbor breaks the agent" outcome: Node exits non-zero on
        // one.
        // Node reports an unhandled rejection at the end of a turn, so give it
        // two turns rather than a duration to guess at.
        for (let turn = 0; turn < 2; turn += 1) {
            const settled = Promise.withResolvers<void>();
            setImmediate(settled.resolve);
            await settled.promise;
        }
        assert.deepEqual(rejections, []);
    } finally {
        process.off("unhandledRejection", onRejection);
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

test("toHookInput normalizes the harness's own event shape", () => {
    // Both spellings are accepted so a harness that renames the field in either
    // direction cannot silently take waiting_input away, and exactly one of
    // them is produced.
    assert.equal(
        toHookInput("tool_call", { toolName: " ask ", input: {} }, "s", "t").tool,
        "ask",
    );
    assert.equal(toHookInput("tool_call", { tool: "ask" }, "s", "t").tool, "ask");
    // A payload that is not an object at all still yields a usable event: the
    // name comes from the registration, not from the payload.
    assert.deepEqual(toHookInput("agent_start", undefined, "s", "t"), {
        event: "agent_start",
        devSessionId: "s",
        terminalId: "t",
    });
    // A failed tool call is ordinary agent work, not a broken agent: turning
    // isError into native.error would paint the row red for every command that
    // exited non-zero.
    assert.equal(
        toHookInput("tool_result", { toolName: "bash", isError: true }, "s", "t").error,
        undefined,
    );
    assert.equal(
        toHookInput("agent_end", { willContinue: true }, "s", "t").willContinue,
        true,
    );
});
