import type { HarnessAdapter } from "./types.ts";
import { mapPiFamilyEvent, piFamilyMetadata } from "./pi-family.ts";

// Oh My Pi is the highest-priority integration (SPEC 6.2). It shares its event
// vocabulary — and therefore its whole SPEC 6.5 mapping, including the
// shutdown-outranks-error precedence — with Pi; both live in pi-family.ts, and
// this file is the harness identity that binds them to "oh-my-pi".
export const ohMyPiAdapter = {
    harness: "oh-my-pi",
    map: mapPiFamilyEvent,
    metadata: piFamilyMetadata,
    // `satisfies` keeps `harness` a literal type so adapters/index.ts can
    // require the registry key to match it; see claude-code.ts.
} satisfies HarnessAdapter;
