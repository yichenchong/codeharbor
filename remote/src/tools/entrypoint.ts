import { realpathSync } from "node:fs";
import { pathToFileURL } from "node:url";

/** Node resolves the main module's symlinks, but leaves argv[1] as invoked. */
export function isEntrypoint(moduleUrl: string): boolean {
    const script = process.argv[1];
    if (script === undefined) return false;
    if (moduleUrl === pathToFileURL(script).href) return true;
    try {
        return moduleUrl === pathToFileURL(realpathSync(script)).href;
    } catch {
        // Importing a module from `node -e` may leave a non-path argument here.
        return false;
    }
}
