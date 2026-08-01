import type { HarnessAdapter } from "./types.ts";
import { mapPiFamilyEvent, piFamilyMetadata } from "./pi-family.ts";

// Pi shares Oh My Pi's event vocabulary (SPEC 6.2), so it shares the mapping
// itself rather than a copy of it: see pi-family.ts. This file is the harness
// identity only. Divergence, if it ever comes, is a case in that one function
// or an adapter that stops calling it.
export const piAdapter: HarnessAdapter = {
    harness: "pi",
    map: mapPiFamilyEvent,
    metadata: piFamilyMetadata,
};
