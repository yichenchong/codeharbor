// codeharbor-view: drive the CodeHarbor viewer panes from a terminal pane.
//
// The shell face of the viewer control channel. The MCP server
// (remote/src/mcp/server.ts) is the surface an AI harness should normally use —
// typed tools, no quoting, and it runs outside a harness's command sandbox — but
// this exists for three things the MCP server cannot do:
//
//   * smoke-testing the whole path by hand, from the very pane an agent runs in;
//   * a harness with no MCP support at all;
//   * telling a broken socket from a broken tool, because it prints the exact
//     refusal the daemon produced.
//
// Exit status is the contract: 0 when the command was applied, 1 when it was
// refused (the JSON on stdout carries the code and message), 2 for a usage
// error. It never throws: a stack trace would be a worse answer than a refusal.

import { isEntrypoint } from "./entrypoint.ts";

import { CONTROL_OPS, MAX_CONTROL_LINE_BYTES, type ControlOp } from "../control.ts";
import { sendControlRequest } from "../control-client.ts";
import { stringifyJson } from "../json.ts";

export const USAGE =
    "usage: codeharbor-view <list|open|close|split|focus|reload> [options]\n" +
    "\n" +
    "  open    --url <address> [--pane <id>] [--kind <handler>] [--new-pane]\n" +
    "  split   --orientation <horizontal|vertical> [--pane <id>]\n" +
    "  close   [--pane <id>]\n" +
    "  focus   [--pane <id>]\n" +
    "  reload  [--pane <id>]\n" +
    "  list\n" +
    "\n" +
    "  --url accepts an http(s):// address, a file:// URL naming a file on THIS\n" +
    "  server, or a remote path (relative paths resolve against the Dev Session's\n" +
    "  repository root). A trailing slash asks for the directory listing.\n" +
    "  --pane defaults to the focused viewer pane; `list` reports the pane ids.\n" +
    "\n" +
    "  Requires OMP_DEV_SESSION_ID and OMP_TERMINAL_ID, which CodeHarbor exports\n" +
    "  into every terminal pane it creates.\n";

export interface ParsedArgv {
    op: ControlOp;
    args: Record<string, unknown>;
}

const OP_OPTIONS: Record<ControlOp, readonly string[]> = {
    list: [],
    open: ["--url", "--pane", "--kind", "--new-pane"],
    close: ["--pane"],
    split: ["--orientation", "--pane"],
    focus: ["--pane"],
    reload: ["--pane"],
};

/**
 * Parse the argument vector after the program name. Throws a plain Error whose
 * message is the usage complaint; main() turns that into exit 2.
 */
export function parseArgv(argv: readonly string[]): ParsedArgv {
    const [op, ...rest] = argv;
    if (op === undefined) throw new Error("no operation given");
    if (!(CONTROL_OPS as readonly string[]).includes(op)) {
        throw new Error(`unknown operation '${op}'`);
    }
    const args: Record<string, unknown> = {};
    for (let i = 0; i < rest.length; i += 1) {
        const flag = rest[i];
        if (!OP_OPTIONS[op as ControlOp].includes(flag)) {
            throw new Error(`option '${flag}' is not valid for ${op}`);
        }
        switch (flag) {
            case "--new-pane":
                args.newPane = true;
                break;
            case "--url":
            case "--pane":
            case "--kind":
            case "--orientation": {
                const value = rest[i + 1];
                if (value === undefined || value.startsWith("--")) {
                    throw new Error(`${flag} needs a value`);
                }
                i += 1;
                // The wire field for --pane is `pane`; the others match their
                // flag. Spelled out rather than derived so a rename of either
                // side is a visible edit here.
                args[flag === "--pane" ? "pane" : flag.slice(2)] = value;
                break;
            }
            default:
                throw new Error(`unknown option '${flag}'`);
        }
    }
    if (op === "open" && args.url === undefined) {
        throw new Error("open requires --url");
    }
    if (op === "split" && args.orientation !== "horizontal" && args.orientation !== "vertical") {
        throw new Error("split requires --orientation horizontal or vertical");
    }
    return { op: op as ControlOp, args };
}

/** Wait for output errors as well as synchronous stream failures. */
function writeOutput(stream: NodeJS.WritableStream, text: string): Promise<boolean> {
    const { promise, resolve } = Promise.withResolvers<boolean>();
    const onError = (): void => { resolve(false); };
    stream.once("error", onError);
    try {
        stream.write(text, (error?: Error | null) => {
            // Failed write callbacks run before the stream's error event.
            // Leave its listener installed until that event consumes it.
            if (!error) stream.off("error", onError);
            resolve(!error);
        });
    } catch {
        stream.off("error", onError);
        resolve(false);
    }
    return promise;
}

/** Run one command and return the process exit code. */
export async function main(
    argv: readonly string[] = process.argv.slice(2),
    env: NodeJS.ProcessEnv = process.env,
    out: NodeJS.WritableStream = process.stdout,
    err: NodeJS.WritableStream = process.stderr,
): Promise<number> {
    let parsed: ParsedArgv;
    try {
        parsed = parseArgv(argv);
    } catch (parseError) {
        await writeOutput(err,
            `codeharbor-view: ${parseError instanceof Error ? parseError.message : String(parseError)}\n${USAGE}`);
        return 2;
    }

    const response = await sendControlRequest(parsed.op, parsed.args, env);
    if (response.ok) {
        return await writeOutput(out, `${stringifyJson(response.data ?? {}, MAX_CONTROL_LINE_BYTES)}\n`) ? 0 : 1;
    }
    // The refusal goes to STDOUT as JSON, not to stderr: it is the command's
    // answer, and a caller that pipes this into `jq` needs it either way. The
    // human-readable copy on stderr is what a person sees in the pane.
    await writeOutput(out, `${stringifyJson(response.error, MAX_CONTROL_LINE_BYTES)}\n`);
    await writeOutput(err, `codeharbor-view: ${response.error?.code}: ${response.error?.message}\n`);
    return 1;
}

if (isEntrypoint(import.meta.url)) {
    process.exitCode = await main();
}
