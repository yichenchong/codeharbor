// The terminal pane's own right-click menu.
//
// WHY THE PAGE OWNS IT. The pane attaches a tmux session that is started with
// mouse reporting on (see ch::TerminalController::tmuxNewSessionCommand), so
// every button press the terminal receives is normally forwarded to tmux as a
// mouse report. tmux's default binding for the right button is `display-menu`,
// which draws a menu of tmux's own INSIDE the terminal grid and closes it again
// on the next mouse report - including the motion reports a moving pointer
// produces. That is the menu that "closes when the mouse moves".
//
// The right button is therefore never reported: index.ts swallows it in the
// capture phase, and this menu takes its place. It is ordinary DOM, so a moving
// pointer cannot disturb it, and it closes on exactly the three actions a
// desktop menu closes on: choosing an item, pressing Escape, and pressing a
// mouse button somewhere else.

/** The commands the menu can run. Values double as `data-action` attributes. */
export type TerminalMenuAction = "copy" | "paste" | "select-all";

export interface TerminalMenuItem {
    action: TerminalMenuAction;
    label: string;
    enabled: boolean;
}

export interface Point {
    x: number;
    y: number;
}

export interface Size {
    width: number;
    height: number;
}

/**
 * The menu's items for the current state of the terminal.
 *
 * Copy is the only entry that depends on anything: with no selection there is
 * nothing to copy, and a menu that offers it anyway leaves the user pressing a
 * command that silently does nothing. Paste is always offered - what is on the
 * clipboard cannot be read without asking for it, and asking on every right
 * click to grey out one item would be worse than a paste that turns out empty.
 */
export function terminalMenuItems(hasSelection: boolean): TerminalMenuItem[] {
    return [
        { action: "copy", label: "Copy", enabled: hasSelection },
        { action: "paste", label: "Paste", enabled: true },
        { action: "select-all", label: "Select All", enabled: true },
    ];
}

/**
 * The hint printed under the items.
 *
 * Selecting with a plain drag is not this application's to give: with mouse
 * reporting on, a drag belongs to the program running in the terminal (tmux, or
 * whatever tmux is showing). xterm.js's own escape hatch is a modifier - Alt on
 * macOS, Shift everywhere else - and the menu is where a user who just tried to
 * select something is looking, so the hint lives here rather than in the manual.
 */
export function selectionHintText(isMac: boolean): string {
    return isMac
        ? "Hold \u2325 Option and drag to select text"
        : "Hold Shift and drag to select text";
}

/**
 * Where to put the menu's top-left corner so that the whole menu stays inside
 * `bounds`, given the point the user clicked.
 *
 * Coordinates are relative to `bounds`, which is the terminal page's own box.
 * A menu opened near the right edge is pulled left rather than clipped, and one
 * taller than the page is pinned to the top so its first item is reachable.
 */
export function clampMenuOrigin(point: Point, size: Size, bounds: Size): Point {
    const x = Math.max(0, Math.min(point.x, bounds.width - size.width));
    const y = Math.max(0, Math.min(point.y, bounds.height - size.height));
    return { x, y };
}

/** What the menu needs from the terminal it belongs to. */
export interface TerminalMenuHost {
    /** Whether the page currently holds a selection (`Terminal.hasSelection`). */
    hasSelection(): boolean;
    /** Run a chosen command. */
    run(action: TerminalMenuAction): void;
    /** Put the keyboard back in the terminal after the menu closes. */
    focusTerminal(): void;
    /** True on macOS, which uses a different modifier for forced selection. */
    isMac: boolean;
}

/**
 * The menu itself: a `div.ch-terminal-menu` inserted into `container` while it
 * is open and removed again when it closes.
 */
export class TerminalContextMenu {
    private readonly _document: Document;
    private readonly _container: HTMLElement;
    private readonly _host: TerminalMenuHost;
    private _root: HTMLElement | null = null;
    private _disposed = false;

    constructor(container: HTMLElement, host: TerminalMenuHost) {
        const ownerDocument = container.ownerDocument;
        if (!ownerDocument) {
            throw new Error("terminal menu container has no document");
        }
        this._document = ownerDocument;
        this._container = container;
        this._host = host;
        // Capture phase, on the document: a press anywhere outside dismisses the
        // menu before the thing underneath acts on it, which is what every
        // platform menu does. Presses on the menu are excluded below.
        this._document.addEventListener("mousedown", this._onDocumentMouseDown, true);
        this._document.addEventListener("keydown", this._onDocumentKeyDown, true);
    }

    get isOpen(): boolean {
        return this._root !== null;
    }

    /** Open at a point in client coordinates (a MouseEvent's clientX/clientY). */
    openAt(clientX: number, clientY: number): void {
        if (this._disposed) {
            return;
        }
        this.close();
        const root = this._document.createElement("div");
        root.className = "ch-terminal-menu";
        root.setAttribute("role", "menu");
        for (const item of terminalMenuItems(this._host.hasSelection())) {
            const button = this._document.createElement("button");
            button.className = "ch-terminal-menu-item";
            button.type = "button";
            button.dataset.action = item.action;
            button.textContent = item.label;
            button.setAttribute("role", "menuitem");
            if (!item.enabled) {
                button.disabled = true;
            }
            // `click` rather than `mousedown`: a press that started on the menu
            // and ended off it must not run the command, and click is the event
            // with that rule already in it.
            button.addEventListener("click", () => {
                this.close();
                this._host.run(item.action);
            });
            root.appendChild(button);
        }
        const hint = this._document.createElement("div");
        hint.className = "ch-terminal-menu-hint";
        hint.textContent = selectionHintText(this._host.isMac);
        root.appendChild(hint);

        // Measured while off screen, so the clamp below works on the menu's real
        // size instead of an estimate: the item labels and the user's font both
        // change it.
        root.style.visibility = "hidden";
        root.style.left = "0px";
        root.style.top = "0px";
        this._container.appendChild(root);
        const containerRect = this._container.getBoundingClientRect();
        const origin = clampMenuOrigin(
            { x: clientX - containerRect.left, y: clientY - containerRect.top },
            { width: root.offsetWidth, height: root.offsetHeight },
            { width: containerRect.width, height: containerRect.height },
        );
        root.style.left = `${origin.x}px`;
        root.style.top = `${origin.y}px`;
        root.style.visibility = "";
        this._root = root;

        // Focus the first item the user can actually run, so Escape and the
        // keyboard reach the menu instead of the terminal underneath it.
        const first = root.querySelector<HTMLButtonElement>(
            ".ch-terminal-menu-item:not([disabled])",
        );
        first?.focus();
    }

    close(): void {
        const root = this._root;
        if (!root) {
            return;
        }
        this._root = null;
        root.remove();
        if (!this._disposed) {
            this._host.focusTerminal();
        }
    }

    dispose(): void {
        if (this._disposed) {
            return;
        }
        this._disposed = true;
        this.close();
        this._document.removeEventListener("mousedown", this._onDocumentMouseDown, true);
        this._document.removeEventListener("keydown", this._onDocumentKeyDown, true);
    }

    private readonly _onDocumentMouseDown = (event: MouseEvent): void => {
        const root = this._root;
        if (!root) {
            return;
        }
        const target = event.target;
        if (target instanceof Node && root.contains(target)) {
            return;
        }
        this.close();
    };

    private readonly _onDocumentKeyDown = (event: KeyboardEvent): void => {
        if (!this._root || event.key !== "Escape") {
            return;
        }
        // Swallowed: with the menu open, Escape belongs to the menu. Left alone
        // it would also reach xterm.js and go to the remote shell as ESC.
        event.preventDefault();
        event.stopPropagation();
        this.close();
    };
}
