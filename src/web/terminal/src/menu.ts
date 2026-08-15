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
// pointer cannot disturb it, and it closes on exactly the ways a desktop menu
// closes: choosing an item, pressing Escape, pressing a mouse button somewhere
// else, and focus leaving it (a Tab out of the menu, or the window losing it).

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
 * A plain drag selects on this page and the highlight stays: the pane inverts
 * xterm.js's rule, whose modifier - Alt on macOS, Shift everywhere else - now
 * hands the drag to the program running in the terminal instead (see
 * mouse.ts). That is the half a user cannot discover by trying, because the
 * thing they tried already worked, so the hint names the modifier and what it
 * gives up rather than how to select.
 */
export function selectionHintText(isMac: boolean): string {
    return isMac
        ? "Drag to select text; hold \u2325 Option to use the mouse in the terminal"
        : "Drag to select text; hold Shift to use the mouse in the terminal";
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

/**
 * Where the arrow keys move inside an open menu. `current` is the index of the
 * focused item, or -1 when focus is on the menu itself rather than on an item;
 * `count` counts only the items a user can actually focus (a disabled item is
 * skipped, which is why this works on a pre-filtered list rather than on the
 * whole menu). Both ends wrap, the way a desktop menu does. Returns null for a
 * key this menu does not claim, so the caller leaves it to the terminal.
 */
export function menuFocusTarget(
    key: string,
    current: number,
    count: number,
): number | null {
    if (count === 0) {
        return null;
    }
    switch (key) {
        case "ArrowDown":
            return current < 0 ? 0 : (current + 1) % count;
        case "ArrowUp":
            return current < 0 ? count - 1 : (current - 1 + count) % count;
        case "Home":
            return 0;
        case "End":
            return count - 1;
        default:
            return null;
    }
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
        // A role=menu with no name is announced as an anonymous menu, which is
        // no help at all in a window that has several. The name says which
        // menu this is, not what it can do — the items do that.
        root.setAttribute("aria-label", "Terminal actions");
        // The menu closes as soon as focus leaves it, the way a desktop menu
        // does: a Tab out of the last item, or the window being deactivated,
        // must not leave a floating menu behind over the terminal. Focus is
        // NOT pulled back to the terminal here — the user is deliberately
        // moving it somewhere else, and yanking it back would trap them.
        root.addEventListener("focusout", (event) => {
            if (this._root !== root) {
                return;
            }
            const next = (event as FocusEvent).relatedTarget;
            if (next instanceof Node && root.contains(next)) {
                return;
            }
            this.close(false);
        });
        for (const item of terminalMenuItems(this._host.hasSelection())) {
            const button = this._document.createElement("button");
            button.className = "ch-terminal-menu-item";
            button.type = "button";
            button.dataset.action = item.action;
            button.textContent = item.label;
            button.setAttribute("role", "menuitem");
            // Roving tabindex: the menu is one stop, and the arrow keys move
            // within it (see _onDocumentKeyDown). Tab therefore leaves the menu
            // rather than walking its items, which is what closes it.
            button.tabIndex = -1;
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

    /**
     * Close the menu. `restoreFocus` hands focus back to the terminal, which is
     * right for every way the user finishes WITH the menu — running an item,
     * Escape, a press elsewhere — and wrong when the menu is closing precisely
     * because focus has already moved somewhere the user chose.
     */
    close(restoreFocus = true): void {
        const root = this._root;
        if (!root) {
            return;
        }
        this._root = null;
        root.remove();
        if (restoreFocus && !this._disposed) {
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
        const root = this._root;
        if (!root) {
            return;
        }
        if (event.key === "Escape") {
            // Swallowed: with the menu open, Escape belongs to the menu. Left
            // alone it would also reach xterm.js and go to the remote shell as
            // ESC.
            event.preventDefault();
            event.stopPropagation();
            this.close();
            return;
        }
        const items = [...root.querySelectorAll<HTMLButtonElement>(
            ".ch-terminal-menu-item:not([disabled])",
        )];
        const target = menuFocusTarget(
            event.key,
            items.indexOf(this._document.activeElement as HTMLButtonElement),
            items.length,
        );
        if (target === null) {
            return;
        }
        // Same reason as Escape: an arrow key that reached xterm.js would be
        // sent to the remote shell as a cursor key while the user was only
        // moving through this menu.
        event.preventDefault();
        event.stopPropagation();
        items[target].focus();
    };
}
