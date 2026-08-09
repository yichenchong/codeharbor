import QtQuick
import QtQuick.Controls.Basic
import "RemotePath.js" as RemotePath

// A single viewer pane (SPEC 3.3, 7.5) — a leaf of the viewer split tree, and a
// BROWSER: it is addressed by a URL, keeps its own address bar and chrome, and
// hands the resolved resource to a HANDLER. It asks the ViewerModel (`viewers`)
// to classify its URL and loads the matching handler view (rendered Markdown,
// Monaco source/text, image, PDF, directory, or binary). No handler owns the
// pane, and an unrecognised resource stays with the browser.
//
// THE PANE IS FILLED FROM HERE. Until now a viewer pane could only be filled by
// something else handing it a URL, and nothing in the application did: the
// sidebar lists Dev Sessions and has no file browser, and the command palette
// carries only split and close commands. So a pane's own header now carries an
// ADDRESS BAR — a remote absolute path or a URL, Enter opens it in this pane —
// an empty pane falls back to LISTING the active Dev Session's repository root
// so there is always something to click, and the directory listing itself is
// navigable (ViewerDirectoryView.openRequested). None of that goes through the
// palette.
//
// file:// ALWAYS means a file on the remote SSH server, never a local one
// (SPEC 8.3), so remotePathOf()/fileUrlFor() below are the only conversion
// between the two spellings and are exact inverses of each other.
Item {
    id: pane

    property url url
    property string paneId: ""

    // This is the pane the user is working in. Written by the region that owns
    // the pane cache (ViewerRegion.applyFocusFlags) rather than derived here:
    // focus is a property of the REGION — exactly one of its panes has it — and
    // a pane cannot see its siblings.
    property bool paneActive: false

    // A restore may arrive before the handler's WebEngineView has finished
    // materialising. Keep the request until a view exists, but cancel it as
    // soon as the region gives focus to another pane so a late page callback
    // cannot fight a user's click.
    property bool focusPending: false
    // The retry is BOUNDED. focusContent() re-arms the timer every time the
    // page is not scriptable yet, and a page that never becomes scriptable —
    // a handler whose load failed, a document Chromium never finished — would
    // otherwise run a JavaScript request against it ten times a second for as
    // long as the pane exists. Twenty seconds is the same budget the path
    // probe and the terminal identity watchdog use; after it, giving up costs
    // only the keyboard focus this restore was trying to place.
    readonly property int focusRetryLimit: 200
    property int focusRetriesLeft: 0
    Timer {
        id: focusRetry
        objectName: "viewerFocusRetry"
        interval: 100
        repeat: false
        onTriggered: pane.focusContent()
    }
    // One place that arms it, so no caller can re-arm past the budget.
    function armFocusRetry() {
        if (pane.focusRetriesLeft <= 0) {
            pane.focusPending = false
            focusRetry.stop()
            return
        }
        pane.focusRetriesLeft -= 1
        focusRetry.restart()
    }
    onPaneActiveChanged: {
        if (!pane.paneActive) {
            pane.focusPending = false
            focusRetry.stop()
        }
    }

    // ---- browser navigation history ----------------------------------------
    //
    // A pane is a browser even when its current handler is a directory, image,
    // PDF, binary download, or editor. The address bar therefore owns the
    // navigation state rather than leaving it to whichever handler happens to
    // be loaded. Keeping the list on this Item is what makes a split and a
    // session-layout re-render harmless: ViewerRegion re-parents this exact
    // object instead of rebuilding it.
    property var navigationHistory: []
    property int navigationIndex: -1
    property bool navigationReady: false
    property bool navigationMoveInProgress: false
    // Toggling this property destroys and recreates the handler Loader on
    // reload. It is intentionally pane-local: a reload must not disturb a
    // sibling pane's WebEngine profile or its own handler state.
    property bool contentActive: true

    readonly property bool canGoBack: pane.navigationIndex > 0
    readonly property bool canGoForward:
        pane.navigationIndex >= 0
        && pane.navigationIndex < pane.navigationHistory.length - 1
    readonly property bool canGoHome: pane.sessionRoot.length > 0
    readonly property bool canReload: pane.effectiveUrl.toString().length > 0

    function defaultAddressForRoot(rootPath) {
        const root = String(rootPath || "");
        if (root.length === 0)
            return "";
        return pane.fileUrlFor(root.charAt(root.length - 1) === "/" ? root : root + "/")
                   .toString();
    }

    // The URL property deliberately stays empty for an untouched pane so the
    // pane still follows the active session's root. History stores the
    // CONCRETE address that was shown, however, so a root entry remains an
    // honest browser location if the active session changes later.
    function currentNavigationAddress() {
        const opened = pane.url.toString();
        return opened.length > 0 ? opened : pane.defaultAddressForRoot(pane.sessionRoot);
    }

    function initializeNavigationHistory() {
        if (pane.navigationHistory.length > 0 || pane.navigationReady === false)
            return;
        pane.navigationHistory = [pane.currentNavigationAddress()];
        pane.navigationIndex = 0;
    }

    // Record only a new navigation. Reload and history traversal deliberately
    // bypass this function, while a fresh address entry or directory-row click
    // arrives through urlChanged and truncates the forward branch here.
    function recordNavigation(address) {
        if (!pane.navigationReady || pane.navigationMoveInProgress)
            return;
        const value = String(address || "");
        const history = pane.navigationHistory.slice(0);
        if (pane.navigationIndex >= 0
                && pane.navigationIndex < history.length
                && history[pane.navigationIndex] === value)
            return;
        if (pane.navigationIndex >= 0 && pane.navigationIndex < history.length - 1)
            history.splice(pane.navigationIndex + 1);
        history.push(value);
        pane.navigationHistory = history;
        pane.navigationIndex = history.length - 1;
    }

    // A default-root entry follows the active session only while the pane is
    // still in its empty-url state. Existing entries remain untouched, which
    // preserves the pane's history across a session switch without turning an
    // old address into a new current entry.
    function refreshDefaultHistory() {
        if (!pane.navigationReady || pane.url.toString().length > 0
                || pane.navigationIndex < 0
                || pane.navigationIndex >= pane.navigationHistory.length)
            return;
        const history = pane.navigationHistory.slice(0);
        history[pane.navigationIndex] = pane.defaultAddressForRoot(pane.sessionRoot);
        pane.navigationHistory = history;
    }

    function goBack() {
        if (!pane.canGoBack)
            return;
        const targetIndex = pane.navigationIndex - 1;
        const target = pane.navigationHistory[targetIndex];
        pane.navigationMoveInProgress = true;
        pane.navigationIndex = targetIndex;
        pane.forcedKind = "";
        pane.url = String(target || "");
        pane.navigationMoveInProgress = false;
    }

    function goForward() {
        if (!pane.canGoForward)
            return;
        const targetIndex = pane.navigationIndex + 1;
        const target = pane.navigationHistory[targetIndex];
        pane.navigationMoveInProgress = true;
        pane.navigationIndex = targetIndex;
        pane.forcedKind = "";
        pane.url = String(target || "");
        pane.navigationMoveInProgress = false;
    }

    function goHome() {
        if (!pane.canGoHome)
            return;
        // Keep an untouched pane in its follow-the-session form. If the user
        // explicitly opened the root, clearing url restores that same visual
        // location while retaining one concrete root entry in history.
        const target = pane.defaultAddressForRoot(pane.sessionRoot);
        if (pane.currentNavigationAddress() === target && pane.url.toString().length === 0)
            return;
        pane.forcedKind = "";
        pane.url = "";
    }

    // Prefer a handler's own reload operation when it has one: directory
    // listings and web pages can re-fetch without throwing away their live
    // object. The Loader fallback covers every other content kind, including
    // internal image/PDF resources and the binary metadata view.
    function reloadCurrent() {
        if (!pane.canReload)
            return;
        const handler = contentLoader.item;
        if (handler && typeof handler.reload === "function") {
            handler.reload();
            return;
        }
        pane.contentActive = false;
        reloadTimer.restart();
    }


    // ---- focus reporting (SPEC 4.5) ----------------------------------------
    // The user is working in THIS pane. ViewerRegion connects this when it
    // mints the pane (takePane) and republishes it as the root region's
    // `focusedPaneId`; that is what the split/close commands target, instead of
    // guessing at the region's first leaf.
    signal paneActivated(string paneId)

    // Find a handler's actual WebEngineView. Viewer handlers wrap the view in
    // small QML items, and the editor's Monaco page keeps its view one level
    // deeper still. The QML item itself having focus is not enough for a page,
    // so the restore walks to the browser surface and focuses its DOM input too.
    function focusableWebView(item) {
        if (!item)
            return null;
        if (typeof item.runJavaScript === "function")
            return item;
        if (item.item) {
            const nested = pane.focusableWebView(item.item);
            if (nested)
                return nested;
        }
        if (item.children) {
            for (let i = 0; i < item.children.length; ++i) {
                const nested = pane.focusableWebView(item.children[i]);
                if (nested)
                    return nested;
            }
        }
        return null;
    }

    // Accept a focus restore from the owning region. This is intentionally not
    // paneActivated: it is programmatic and must not be mistaken for a user's
    // click while Main.qml is guarding against late restores.
    function acceptFocus() {
        pane.focusPending = true;
        // A fresh restore gets the whole budget back; the previous one is over.
        pane.focusRetriesLeft = pane.focusRetryLimit;
        pane.forceActiveFocus();
        pane.focusContent();
    }

    function focusContent() {
        if (!pane.focusPending || !pane.paneActive)
            return;
        const handler = contentLoader.item;
        if (handler && typeof handler.forceActiveFocus === "function")
            handler.forceActiveFocus();
        const view = pane.focusableWebView(handler);
        if (!view) {
            if (handler)
                pane.focusPending = false;
            return;
        }
        // A Loader becomes ready before its WebEngine page does. Keep the
        // request pending until JavaScript can actually run; otherwise a
        // session restore clears it on an empty document and the editor or
        // terminal-like input never receives keyboard focus.
        view.runJavaScript(
            "(function () {"
          + "try {"
          + "var e = document.querySelector('.monaco-editor textarea.inputarea,"
          + " .xterm-helper-textarea');"
          + "if (e) e.focus();"
          + "return document.readyState;"
          + "} catch (e) { return 'loading'; }"
          + "})()",
            function(result) {
                if (!pane.focusPending || !pane.paneActive)
                    return;
                const state = String(result === undefined || result === null
                                     ? "" : result);
                if (state.length === 0 || state === "loading") {
                    // Deliberately does NOT re-arm: the unconditional
                    // armFocusRetry() below already spent one unit of the
                    // budget and restarted the timer for this attempt. Arming
                    // again here charged every attempt TWICE, halving the
                    // twenty-second window the comment on `focusRetryLimit`
                    // promises.
                    return;
                }
                pane.focusPending = false;
            });
        // The callback is asynchronous and a page can take several turns to
        // start executing scripts. This also covers engines that do not invoke
        // a callback for a request issued before navigation completes.
        pane.armFocusRetry();
    }

    Connections {
        target: contentLoader
        function onLoaded() { pane.focusContent(); }
    }

    // ---- content reporting (SPEC 4.5) --------------------------------------
    // What this pane has open. The host records it in this pane's split-tree
    // leaf (SessionLayouts::setPaneUrl), which is what makes reopening a Dev
    // Session restore the FILES the user had open and not just the geometry.
    //
    // Also emitted on a paneId change, because a pane is born before it has a
    // name: ViewerRegion.takePane() creates the Item WITH its url and assigns
    // paneId immediately afterwards, so a url-only report would arrive
    // anonymous. That birth report is pure echo — the url came out of the very
    // leaf the host has stored — and the region deliberately subscribes AFTER
    // naming the pane, so nobody hears it; see the ordering comment in
    // ViewerRegion.takePane(). What the host needs is every later CHANGE, and
    // this signal is how those arrive.
    signal urlOpened(string paneId, string url)

    // A directory row chose a target for another viewer pane. The region relays
    // this to Main.qml, which calls SessionLayouts::splitPane and therefore
    // preserves the existing pane-identity/persistence path.
    signal openInNewPaneRequested(string paneId, string url, string kind)

    // Shell-level failures (invalid custom schemes, or a desktop with no
    // registered handler) use the same toast path as layout/controller errors.
    signal messageRequested(string message)

    // The pane header's split and close actions name this pane explicitly. The
    // region relays both requests to Main.qml under its session/layout stamp.
    signal splitRequested(string paneId, string orientation)
    signal closeRequested(string paneId)
    function reportUrl() {
        // An unnamed pane is not addressable. The empty paneId is also a real
        // key (the placeholder leaf of an emptied region), but the two are
        // indistinguishable here, and reporting a url against a tree that has
        // no empty leaf would only raise a spurious host error.
        if (pane.paneId.length === 0)
            return;
        // `url`, deliberately NOT `effectiveUrl`: the session-root listing an
        // empty pane falls back to is a DEFAULT, not something the user opened,
        // and persisting it would turn every empty pane into a pinned directory
        // that no longer follows the Dev Session.
        pane.urlOpened(pane.paneId, pane.url.toString());
    }
    onDefaultUrlChanged: {
        pane.refreshDefaultHistory()
        pane.syncAddressField(false)
    }
    // A navigation is the pane's OWN answer to whatever was asked of it, so the
    // field follows it even while it has the keyboard: the user pressed Enter on
    // a relative name ("README.md" typed in a listing, or a path the server had
    // to be asked about first), and leaving that fragment sitting in the bar
    // would have the pane claim to be showing something it is not.
    onUrlChanged: {
        pane.recordNavigation(pane.currentNavigationAddress());
        pane.reportUrl();
        pane.syncAddressField(true);
    }
    onPaneIdChanged: pane.reportUrl()


    // ---- remote paths <-> file:// URLs -------------------------------------

    // The remote path inside a file:// URL; anything else (http/https) comes
    // back unchanged, because that IS its address. The rule itself lives in
    // RemotePath.js, which is the module's ONE copy of it (QM17).
    function remotePathOf(u) {
        return RemotePath.fileUrlToPath(u.toString());
    }

    // The exact inverse of remotePathOf(), and of ViewerDirectoryView's
    // remotePath(): see RemotePath.pathToFileUrl() for why every segment is
    // percent-encoded individually.
    function fileUrlFor(path) {
        return RemotePath.pathToFileUrl(path);
    }

    // Open a remote path in THIS pane. A trailing slash means a directory, which
    // is what ViewerHandlerRegistry::resolve() reads to pick the listing view.
    function openRemotePath(path) {
        pane.forcedKind = "";
        pane.url = pane.fileUrlFor(path);
    }
    // A link inside a WebEngine handler is still ordinary pane navigation.
    // Remote file links become the pane's canonical file:// address; HTTP(S)
    // links switch to the external-profile web handler.
    function openWebNavigation(address) {
        const target = String(address || "");
        if (target === pane.url.toString())
            return;
        if (/^file:\/\//i.test(target) || /^https?:\/\//i.test(target)) {
            pane.forcedKind = "";
            pane.url = target;
        }
    }


    function normalizedForcedKind(kind) {
        switch (String(kind)) {
        case "editor":
        case "text":
            return "text";
        // The rendered Markdown document is a handler in its own right, so
        // "Open as -> Markdown" has to be accepted here as well as
        // "Open as -> Editor", which is how the SOURCE of a .md file is read.
        case "markdown":
        case "image":
        case "pdf":
        case "binary":
        case "directory":
        case "web":
            return String(kind);
        default:
            return "";
        }
    }

    // Apply an explicit handler choice to a URL. The registry remains the
    // source of defaults; this override exists only for the user action that
    // requested "Open as".
    function openUrlWithKind(targetUrl, requestedKind) {
        const selected = pane.normalizedForcedKind(requestedKind);
        if (selected.length === 0) {
            pane.messageRequested(qsTr("That viewer cannot display this target."));
            return false;
        }
        pane.forcedKind = selected;
        pane.url = targetUrl;
        return true;
    }

    function requestOpenAs(path, requestedKind, inNewPane) {
        const targetUrl = pane.fileUrlFor(path);
        if (inNewPane) {
            pane.openInNewPaneRequested(pane.paneId, targetUrl, requestedKind);
            return;
        }
        pane.openUrlWithKind(targetUrl, requestedKind);
    }

    function requestOpenWith(path, scheme) {
        if (!pane.viewerModel
                || typeof pane.viewerModel.isValidApplicationScheme !== "function"
                || !pane.viewerModel.isValidApplicationScheme(scheme)) {
            pane.messageRequested(qsTr("Invalid application scheme."));
            return;
        }
        if (typeof pane.viewerModel.openWithApplication !== "function"
                || !pane.viewerModel.openWithApplication(scheme, path)) {
            pane.messageRequested(qsTr("No desktop application accepted %1://.")
                                  .arg(scheme));
        }
    }

    // ---- the session's repository root, as an empty pane's default ---------

    // `app` is a root context property (AppController). Guarded with typeof
    // exactly as TerminalPaneView guards `terminalFactory`: an unguarded lookup
    // of a missing context property THROWS, which would abort the whole binding
    // pass that is building the pane, and a bare QML load (tst_uxshell) installs
    // no `app`.
    readonly property string sessionRoot:
        (typeof app !== "undefined" && app && app.activeSessionRepoRoot)
        ? String(app.activeSessionRepoRoot) : ""

    // The session root as a directory URL, or empty when there is no active Dev
    // Session — in which case the pane keeps a real empty state.
    readonly property url defaultUrl: {
        const root = pane.sessionRoot;
        if (root.length === 0)
            return "";
        return pane.fileUrlFor(root.charAt(root.length - 1) === "/" ? root : root + "/");
    }

    readonly property bool showingDefault: pane.url.toString().length === 0
                                           && pane.defaultUrl.toString().length > 0

    // What the sub-views actually render: what the user opened, else the session
    // root listing.
    readonly property url effectiveUrl: pane.url.toString().length > 0
                                        ? pane.url : pane.defaultUrl

    // The `viewers` context property (ch::ViewerModel), resolved once and
    // guarded for the same reason `app` is above: this pane's chrome — the
    // header, the address bar — exists whether or not there is anything open, so
    // the lookup is no longer confined to a branch that only runs once a URL has
    // arrived. An unguarded lookup of a missing context property throws, which
    // would abort the binding pass building the pane.
    readonly property var viewerModel: (typeof viewers !== "undefined") ? viewers : null

    // "Open as" is a transient handler override. The persisted URL remains
    // canonical, so reopening a session still follows the registry's default;
    // ordinary navigation clears this override before assigning a new URL.
    property string forcedKind: ""

    readonly property bool contentDirty:
        contentLoader.item && contentLoader.item.dirty === true

    // View kind for the current URL: "web" | "markdown" | "text" | "image" |
    // "pdf" | "directory" | "binary" (nothing to show -> a neutral placeholder).
    property string kind: {
        if (pane.forcedKind.length > 0)
            return pane.forcedKind;
        if (pane.effectiveUrl.toString().length === 0 || !pane.viewerModel)
            return "empty";
        // Invokable calls do not establish a QML binding dependency by
        // themselves. Reading this notify-backed revision makes already-open
        // panes re-evaluate when the user changes a default.
        pane.viewerModel.viewKindsRevision;
        return pane.viewerModel.viewKind(pane.effectiveUrl);
    }
    // The address as the user reads it: a remote path for a file:// URL
    // and the address itself for anything else.
    readonly property string displayPath: pane.remotePathOf(pane.effectiveUrl)

    // Last segment of the address, which is what identifies a pane at a glance.
    // A directory keeps its trailing slash so a header cannot read as a file.
    readonly property string displayName: {
        const p = pane.displayPath;
        if (p.length === 0)
            return "";
        const isDir = p.charAt(p.length - 1) === "/";
        const trimmed = isDir ? p.substring(0, p.length - 1) : p;
        const i = trimmed.lastIndexOf("/");
        const name = i >= 0 ? trimmed.substring(i + 1) : trimmed;
        if (name.length === 0)
            return p;
        return isDir ? name + "/" : name;
    }

    // The directory a RELATIVE address is resolved against, so typing a bare
    // file name in a directory listing opens that file. Empty when the pane is
    // not showing a remote path at all (an http page has no remote directory).
    readonly property string currentDirectory: {
        const p = pane.displayPath;
        if (p.length === 0 || p.charAt(0) !== "/")
            return "";
        if (p.charAt(p.length - 1) === "/")
            return p.length > 1 ? p.substring(0, p.length - 1) : "";
        const i = p.lastIndexOf("/");
        return i <= 0 ? "" : p.substring(0, i);
    }

    // A translated word for what kind of thing is open, shown beside the name.
    readonly property string kindLabel: {
        switch (pane.kind) {
        case "directory": return qsTr("directory");
        case "web": return qsTr("web page");
        case "image": return qsTr("image");
        case "pdf": return qsTr("PDF");
        case "binary": return qsTr("binary");
        case "markdown": return qsTr("Markdown");
        case "text": return qsTr("editor");
        default: return "";
        }
    }

    // ---- outside the repository root (SPEC 9) ------------------------------
    //
    // A path outside the Dev Session's repository root is ALLOWED and stays
    // fully openable — SPEC 9 requires exactly that — but the UI has to SAY so.
    // A Dev Session exists to scope work to one repository, and every path here
    // lives on a remote server where the user has no file manager giving them
    // context, so an out-of-project file that looks identical to an in-project
    // one invites edits to the wrong file. Nothing below confines anything; it
    // only labels.
    //
    // Tri-state, because "not determined yet" is a real answer and must show
    // NOTHING rather than guess either way: "" (unknown) | "inside" | "outside".
    property string repoRootState: ""

    readonly property bool outsideRepository: pane.repoRootState === "outside"

    // The path worth asking about: a remote path, and only when there is a Dev
    // Session for it to be outside OF. displayPath is the address ITSELF for an
    // https:// page or an internal URL, neither of which starts with "/", so
    // those ask nothing and stay unmarked — a web page is not "outside the
    // repository root" and marking it would be noise. An empty pane has an
    // empty displayPath and likewise asks nothing.
    readonly property string repoCheckTarget:
        (pane.sessionRoot.length > 0 && pane.displayPath.length > 0
         && pane.displayPath.charAt(0) === "/") ? pane.displayPath : ""

    // What the outstanding file.resolvePath was asked about, and what a reply is
    // matched against so an answer about a path the pane has already navigated
    // away from cannot label the new one. The base is kept too: the answer
    // depends on it as much as on the path, so switching Dev Session re-asks
    // even while the pane keeps showing the same file.
    property string repoCheckPath: ""
    property string repoCheckBase: ""

    // Ask, unless this exact question is already asked and answered. The guard
    // is what lets the call be driven from several handlers (and from
    // Component.onCompleted, since a pane can be born already showing a file)
    // without spending a round trip per handler.
    function checkRepoRoot() {
        if (pane.repoCheckTarget === pane.repoCheckPath
                && pane.sessionRoot === pane.repoCheckBase)
            return;
        pane.repoCheckPath = pane.repoCheckTarget;
        pane.repoCheckBase = pane.sessionRoot;
        // Whatever was known was known about a different path.
        pane.repoRootState = "";
        if (pane.repoCheckPath.length === 0 || !pane.viewerModel)
            return;
        pane.viewerModel.resolvePath(pane.repoCheckPath, pane.repoCheckBase);
    }

    onRepoCheckTargetChanged: Qt.callLater(pane.checkRepoRoot)
    // The session-root binding is refreshed after its change notification.
    // Defer until that pass completes so checkRepoRoot compares the new base
    // with the new derived target, not a stale target from the old session.
    onSessionRootChanged: Qt.callLater(pane.checkRepoRoot)

    // ---- address entry -----------------------------------------------------

    // A path typed WITHOUT a trailing slash is ambiguous: only the server knows
    // whether /srv/repos/app is a directory or a file, and guessing wrong shows
    // a directory as an undownloadable "binary file". So the path is offered to
    // file.listDirectory first, and the answer decides: a listing means it was a
    // directory (re-opened with the trailing slash the handler registry needs),
    // an error means it was not and it is opened as a file — whose own view then
    // reports the real failure if there is one. This holds the path being asked
    // about; empty when no probe is outstanding.
    property string probePath: ""

    // The probe is BOUNDED. Nothing else un-wedges it: the header is drawn busy
    // for as long as `probePath` is set, and the two replies that clear it are
    // the only things that ever did. A file.listDirectory that never answers —
    // an SSH session that dropped between the request and its reply, or a
    // server that accepted the call and died — therefore left the pane spinning
    // on an address the user could no longer submit again (submitAddress would
    // just re-arm the same wait). Same shape and the same budget as
    // TerminalPaneView's identity watchdog: generous next to one round trip, so
    // a merely slow link never trips it, short enough that the pane stops lying
    // while the user is still looking at it.
    //
    // Giving up opens the path as a FILE, exactly as onDirectoryError does: it
    // is the answer that shows the user something, and the file's own view
    // reports the real failure if the link is genuinely gone.
    Timer {
        id: probeWatchdog
        // Named so a test can shorten the wait instead of sitting out twenty
        // real seconds; the same seam every other named item here is.
        objectName: "viewerProbeWatchdog"
        interval: 20000
        repeat: false
        onTriggered: {
            const path = pane.probePath;
            pane.probePath = "";
            if (path.length > 0)
                pane.openRemotePath(path);
        }
    }

    // One place that arms and disarms it, so no settle path can forget: every
    // route out of a probe clears `probePath`.
    onProbePathChanged: {
        if (pane.probePath.length > 0)
            probeWatchdog.restart();
        else
            probeWatchdog.stop();
    }

    // Resolve whatever is in the address field and open it.
    function submitAddress() {
        const text = addressField.text.trim();
        if (text.length === 0) {
            // Emptying the address clears the pane, which is how a user gets
            // back to the session-root default.
            pane.probePath = "";
            pane.forcedKind = "";
            pane.url = "";
            return;
        }
        // An explicit scheme is an address in its own right (https://…, and a
        // file:// URL a user pasted out of this very field).
        if (/^[a-zA-Z][a-zA-Z0-9+.\-]*:\/\//.test(text)) {
            pane.probePath = "";
            pane.forcedKind = "";
            pane.url = text;
            return;
        }
        const path = text.charAt(0) === "/"
                     ? text
                     : pane.currentDirectory + "/" + text;
        if (path.charAt(path.length - 1) === "/" || !pane.viewerModel) {
            // Already spelled as a directory, or there is nobody to ask;
            // nothing to probe.
            pane.probePath = "";
            pane.openRemotePath(path);
            return;
        }
        pane.probePath = path;
        pane.viewerModel.listDirectory(path);
    }

    Connections {
        target: pane.viewerModel
        // Only ever the pane's OWN outstanding probe: `viewers` is shared by
        // every pane, and the directory views listen on this same pair.
        function onDirectoryListed(path, list) {
            if (path !== pane.probePath)
                return;
            pane.probePath = "";
            pane.openRemotePath(path + "/");
        }
        function onDirectoryError(path, message) {
            if (path !== pane.probePath)
                return;
            pane.probePath = "";
            pane.openRemotePath(path);
        }
        // Same rule as the probe above: `viewers` is shared by every pane, so a
        // reply is only this pane's if it names the path this pane asked about.
        function onPathResolved(path, resolvedPath, insideRepositoryRoot) {
            if (path !== pane.repoCheckPath)
                return;
            pane.repoRootState = insideRepositoryRoot ? "inside" : "outside";
        }
        function onPathResolveError(path, message) {
            if (path !== pane.repoCheckPath)
                return;
            // Undetermined. Shown as nothing at all: claiming "inside" would
            // hide a real out-of-project file and claiming "outside" would
            // slander an ordinary one.
            pane.repoRootState = "";
        }
    }

    // Put the cursor in the address bar. Reached from the header title, so
    // clicking the file name is a way in as well as the field itself.
    function focusAddress() {
        addressField.forceActiveFocus();
        addressField.selectAll();
    }

    function requestClose() {
        const handler = contentLoader.item;
        if (handler && handler.dirty === true) {
            closeEditorDialog.open();
            return;
        }
        pane.closeRequested(pane.paneId);
    }

    // The address as the field should read it RIGHT NOW.
    //
    // COMPUTED, deliberately, rather than read off `displayPath`. That property
    // is bound to `effectiveUrl`, which is bound to `url` — and the handler
    // below runs from the very notification that refreshes those bindings.
    // Whether they have been refreshed BEFORE a given handler runs is an
    // ordering detail of the QML engine, not something this pane may depend on:
    // reading `displayPath` from inside onUrlChanged handed back the PREVIOUS
    // address, so the field showed the directory the pane had just left.
    // (TerminalPaneView.awaitingIdentity() is a function for the same reason.)
    // The rule here is the one `effectiveUrl` states: what the user opened,
    // else the session-root default.
    function shownPath() {
        return pane.remotePathOf(pane.url.toString().length > 0 ? pane.url : pane.defaultUrl);
    }

    // The field shows the pane's address, but it is EDITABLE, so a plain
    // binding cannot be used: the first keystroke would break it and the field
    // would then never follow a navigation again. It is pushed instead, and
    // never on top of what the user is in the middle of typing.
    //
    // `addressField` is null-checked because a pane is BORN with a url
    // (ViewerRegion.takePane passes it as an initial property), so these
    // handlers can run before this pane's own children exist; the
    // Component.onCompleted below is what pushes the first value.
    function syncAddressField(force) {
        if (addressField && (force || !addressField.activeFocus))
            addressField.text = pane.shownPath();
    }

    Component.onCompleted: {
        // The effective address may come from the active session's repository
        // root even while `url` is empty. Use the same computed value as every
        // later refresh; reading displayPath here would leave a newly-created
        // pane's address bar blank until the root changed again.
        addressField.text = pane.shownPath();
        pane.navigationReady = true;
        pane.initializeNavigationHistory();
        pane.checkRepoRoot();
    }

    // ---- chrome ------------------------------------------------------------

    AppPaneHeader {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top

        title: pane.displayName.length > 0 ? pane.displayName : qsTr("Empty pane")
        active: pane.paneActive
        subtitle: pane.showingDefault ? qsTr("session root")
                                     : pane.kindLabel
                                       + (pane.contentDirty
                                          ? qsTr(" \u00b7 unsaved changes") : "")
        // fetching. Every handler that can wait on the network publishes
        // `loading` (the directory listing, and the four WebEngine-backed
        // views); the ones that cannot — the binary metadata card, the empty
        // placeholder — simply have no such property, so the test is false for
        // them. Asking only the DIRECTORY view (which is what this used to do)
        // left an image, a PDF, a rendered Markdown document and a web page
        // showing a blank rectangle with nothing anywhere saying they were on
        // their way.
        busy: pane.probePath.length > 0
              || (contentLoader.item && contentLoader.item.loading === true)

        onTitleActivated: pane.focusAddress()

        actions: [
            AppPaneHeader.Action {
                objectName: "viewerSplitHorizontalButton"
                toolbarId: "pane.split.horizontal"
                text: qsTr("Split this pane side by side")
                glyph: "\u25eb"
                onClicked: {
                    pane.paneActivated(pane.paneId);
                    pane.splitRequested(pane.paneId, "horizontal");
                }
            },
            AppPaneHeader.Action {
                objectName: "viewerSplitVerticalButton"
                toolbarId: "pane.split.vertical"
                text: qsTr("Split this pane top and bottom")
                glyph: "\u229f"
                onClicked: {
                    pane.paneActivated(pane.paneId);
                    pane.splitRequested(pane.paneId, "vertical");
                }
            },
            AppPaneHeader.Action {
                objectName: "viewerCloseButton"
                toolbarId: "pane.close"
                text: qsTr("Close this pane")
                glyph: "\u00d7"
                onClicked: {
                    // Report focus first: the host closes the pane this
                    // request names, not a remembered fallback pane.
                    pane.paneActivated(pane.paneId);
                    pane.requestClose();
                }
            }
        ]
    }

    // The address bar. Deliberately always visible rather than hidden behind a
    // click on the title: it is the ONE affordance that makes a viewer pane
    // usable, and an affordance the user has to discover is what the command
    // palette already was.
    Rectangle {
        id: addressBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        height: Theme.paneHeaderHeight
        color: Theme.surfaceDeep

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.borderSubtle
        }

        TextField {
            id: addressField
            objectName: "viewerAddressField"

            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.right: outsideMarker.left
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            height: parent.height - 6

            placeholderText: qsTr("Remote path (/srv/repos/app/README.md) or https:// address")
            placeholderTextColor: Theme.textDim
            color: Theme.text
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeBody
            selectByMouse: true
            leftPadding: 8
            rightPadding: 8
            topPadding: 0
            bottomPadding: 0

            background: Rectangle {
                color: Theme.surfaceSunken
                radius: Theme.radiusSmall
                border.width: addressField.activeFocus ? 2 : 1
                border.color: addressField.activeFocus ? Theme.accent : Theme.borderSubtle
            }

            onAccepted: pane.submitAddress()

            // Escape abandons the edit and puts the pane's real address back,
            // so a half-typed path cannot be left sitting in the field looking
            // like what the pane is showing.
            Keys.onEscapePressed: function (event) {
                addressField.text = pane.displayPath;
                addressField.deselect();
                pane.forceActiveFocus();
                event.accepted = true;
            }
        }

        // The SPEC 9 marker. Quiet by design — beside the address rather than a
        // banner, and in no way blocking: being outside the repository root is
        // a legitimate state the user is allowed to be in, just an easy one to
        // be in by accident. Zero-width while hidden, so an in-project pane's
        // address field is exactly as wide as it was before.
        Rectangle {
            id: outsideMarker
            objectName: "viewerOutsideRepoMarker"

            // The full sentence, said once: the hover tooltip and the screen
            // reader description are the same words.
            readonly property string hint:
                qsTr("This path is outside the project's repository root.")

            visible: pane.outsideRepository
            anchors.right: navigationControls.left
            anchors.verticalCenter: parent.verticalCenter
            width: visible ? outsideLabel.implicitWidth + 12 : 0
            height: 18
            radius: Theme.radiusSmall
            color: "transparent"
            border.width: 1
            border.color: Theme.warning

            // The chip is colour plus three small words; neither reaches a
            // screen reader on its own, exactly as the sidebar's status dot
            // does not (see the Accessible block in SessionRow.qml).
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Outside the project")
            Accessible.description: outsideMarker.hint

            HoverHandler { id: outsideHover }

            Label {
                id: outsideLabel
                anchors.centerIn: parent
                textFormat: Text.PlainText
                text: qsTr("outside project")
                color: Theme.warning
                font.pixelSize: Theme.fontSizeSmall
            }

            // The module's one tooltip (AppToolTip.qml), for the reason
            // AppPaneHeader.Action gives: the attached form is drawn by the
            // Basic style in that style's own light palette, so a hint about
            // dark chrome arrives as a white box.
            AppToolTip {
                id: outsideTip
                x: 0
                y: outsideMarker.height + 4
                text: outsideMarker.hint
                visible: outsideHover.hovered
            }
        }
        // Enter remains the submit path; this row uses the field's trailing
        // chrome for browser navigation without making the keyboard contract
        // pointer-dependent.
        Row {
            id: navigationControls
            objectName: "viewerNavigationControls"
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            Repeater {
                model: ToolbarRegistry.ordered(
                    ["nav.back", "nav.forward", "nav.reload", "nav.home"])

                delegate: Loader {
                    required property string modelData
                    sourceComponent: {
                        switch (modelData) {
                        case "nav.back": return backAction;
                        case "nav.forward": return forwardAction;
                        case "nav.reload": return reloadAction;
                        case "nav.home": return homeAction;
                        default: return null;
                        }
                    }
                }
            }
        }

        Component {
            id: backAction
            AppPaneHeader.Action {
                id: backButton
                objectName: "viewerBackButton"
                toolbarId: "nav.back"
                text: qsTr("Back")
                glyph: "\u2190"
                enabled: pane.canGoBack
                focusPolicy: enabled ? Qt.StrongFocus : Qt.NoFocus
                Accessible.role: Accessible.Button
                Accessible.name: text
                onClicked: pane.goBack()
            }
        }

        Component {
            id: forwardAction
            AppPaneHeader.Action {
                id: forwardButton
                objectName: "viewerForwardButton"
                toolbarId: "nav.forward"
                text: qsTr("Forward")
                glyph: "\u2192"
                enabled: pane.canGoForward
                focusPolicy: enabled ? Qt.StrongFocus : Qt.NoFocus
                Accessible.role: Accessible.Button
                Accessible.name: text
                onClicked: pane.goForward()
            }
        }

        Component {
            id: reloadAction
            AppPaneHeader.Action {
                id: reloadButton
                objectName: "viewerReloadButton"
                toolbarId: "nav.reload"
                text: qsTr("Reload")
                glyph: "\u21bb"
                enabled: pane.canReload
                focusPolicy: enabled ? Qt.StrongFocus : Qt.NoFocus
                Accessible.role: Accessible.Button
                Accessible.name: text
                onClicked: pane.reloadCurrent()
            }
        }

        Component {
            id: homeAction
            AppPaneHeader.Action {
                id: homeButton
                objectName: "viewerHomeButton"
                toolbarId: "nav.home"
                text: qsTr("Home")
                glyph: "\u2302"
                enabled: pane.canGoHome
                focusPolicy: enabled ? Qt.StrongFocus : Qt.NoFocus
                Accessible.role: Accessible.Button
                Accessible.name: text
                onClicked: pane.goHome()
            }
        }
    }

    AppDialog {
        id: closeEditorDialog
        objectName: "viewerCloseUnsavedDialog"
        title: qsTr("Unsaved changes")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        closePolicy: Popup.NoAutoClose
        onOpened: {
            const closeButton = closeEditorDialog.standardButton(Dialog.Ok);
            if (closeButton)
                closeButton.text = qsTr("Close without saving");
        }
        onAccepted: pane.closeRequested(pane.paneId)

        Column {
            width: 380
            spacing: 8
            Label {
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: qsTr("This editor has unsaved changes. Close it without saving?")
                color: Theme.text
            }
        }
    }

    Timer {
        id: reloadTimer
        interval: 0
        repeat: false
        onTriggered: pane.contentActive = true
    }

    Loader {
        id: contentLoader
        active: pane.contentActive
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: addressBar.bottom
        anchors.bottom: parent.bottom
        sourceComponent: {
            switch (pane.kind) {
            case "web": return webComponent;
            case "image": return imageComponent;
            case "pdf": return pdfComponent;
            case "directory": return directoryComponent;
            case "binary": return binaryComponent;
            case "empty": return emptyComponent;
            // Markdown is rendered through its own sanitising page; text stays
            // on Monaco so "Open as -> Editor" remains the source-edit path.
            case "markdown": return markdownComponent;
            case "text": return editorComponent;
            default:
                // Unreachable: ViewerModel::viewKind() answers with exactly the
                // seven words above, and this pane supplies "empty" itself. Kept
                // as the metadata/download view all the same, never as a text
                // handler: SPEC 3.3 makes this pane a BROWSER that delegates to
                // handlers, so an unrecognised resource stays with the browser
                // and gets the download/metadata treatment (SPEC 7.5) rather
                // than being poured into the editor's read-only twin. A text
                // handler is only ever reached on a positive match.
                return binaryComponent;
            }
        }
    }

    // A CLICK is the only reliable evidence that the user is working here. Most
    // of the sub-views above are WebEngineViews, and focus inside a page is
    // Chromium's own state: it never surfaces as QML activeFocus, so watching
    // activeFocus would see nothing for exactly the panes that matter most (an
    // editor buffer being typed into). A click, by contrast, is what PUT the
    // focus in that page, and it is delivered as a real Qt press first.
    //
    // The press is observed and then DECLINED, so it goes on to whatever is
    // underneath — the web view, a header button, the address field, a text view
    // — and this stays a sniffer rather than an input-eating overlay. Declared
    // after the chrome and the content because delivery is topmost-first: a
    // sniffer underneath them would never be reached.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        // This area only observes presses; it must not touch the cursor. Qt
        // documents `cursorShape: undefined` as "do not change the shape on
        // entering this area", which is exactly the behaviour wanted: the
        // cursor of whatever is underneath - a web page's own CSS cursor, the
        // editor's I-beam - reaches the user instead of this full-pane click
        // sniffer's arrow. hoverEnabled stays off for the same reason.
        cursorShape: undefined
        hoverEnabled: false
        onPressed: function(mouse) {
            pane.paneActivated(pane.paneId);
            mouse.accepted = false;
        }
    }

    Component {
        id: webComponent
        ViewerWebView {
            url: pane.effectiveUrl
            remoteFile: pane.effectiveUrl.toString().indexOf("file://") === 0
            onNavigated: (address) => pane.openWebNavigation(address)
        }
    }
    Component { id: imageComponent; ViewerImageView { url: pane.effectiveUrl } }
    Component { id: pdfComponent; ViewerPdfView { url: pane.effectiveUrl } }
    Component {
        id: directoryComponent
        ViewerDirectoryView {
            url: pane.effectiveUrl
            // Clicking a row navigates THIS pane: into a sub-directory, up to a
            // parent, or into a file's own viewer.
            onOpenRequested: (path) => pane.openRemotePath(path)
            onOpenAsRequested: (path, kind, inNewPane) =>
                pane.requestOpenAs(path, kind, inNewPane)
            onOpenWithRequested: (path, scheme) =>
                pane.requestOpenWith(path, scheme)
            onMessageRequested: (message) => pane.messageRequested(message)
        }
    }
    Component { id: binaryComponent; ViewerBinaryView { url: pane.effectiveUrl } }
    Component {
        id: markdownComponent
        ViewerMarkdownView {
            url: pane.effectiveUrl
            onNavigated: (address) => pane.openWebNavigation(address)
        }
    }
    Component { id: editorComponent; EditorPaneView { fileUrl: pane.effectiveUrl; recoveryPaneId: pane.paneId } }

    // Nothing open AND no Dev Session to fall back on. A pane a user can land on
    // must say what it is and how to fill it — the internal pane id is plumbing,
    // and printing it as the headline (which this used to do) tells nobody
    // anything.
    Component {
        id: emptyComponent
        Rectangle {
            color: Theme.surfaceDeep

            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 340)
                spacing: 8

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "\u25a7"
                    color: Theme.textFaint
                    font.pixelSize: 30
                }
                Label {
                    objectName: "emptyTitle"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Nothing open in this pane")
                    color: Theme.text
                    font.pixelSize: Theme.fontSizeTitle
                }
                Label {
                    objectName: "emptyHint"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    // What actually works, which the old text did not describe:
                    // it pointed at a file browser in the sidebar and at palette
                    // commands, and neither exists.
                    text: qsTr("Type a remote path or an https:// address in the bar above and "
                               + "press Enter. Open a Dev Session and this pane starts at its "
                               + "repository root instead.")
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSizeBody
                }
                Label {
                    objectName: "emptyPaneId"
                    anchors.horizontalCenter: parent.horizontalCenter
                    // The id still has to be reachable for a bug report; it is
                    // just no longer the message. Never markup: pane ids are
                    // built from server-supplied session ids.
                    textFormat: Text.PlainText
                    text: pane.paneId
                    visible: pane.paneId.length > 0
                    color: Theme.textFaint
                    font.pixelSize: Theme.fontSizeSmall
                    font.family: Theme.monoFamily
                }
            }
        }
    }
}
