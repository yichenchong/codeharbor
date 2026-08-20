import QtQuick
import QtQuick.Controls
import CodeHarbor.Mobile

// Native Markdown viewer (mobile SPEC 7.5 / 7.4 / 2.4).
//
// SECURITY IS THE WHOLE DESIGN. On the desktop a markdown pane is HTML rendered
// by Chromium in the privileged internal profile behind a restrictive CSP. There
// is no Chromium on Android or iOS, so there is no sandbox — and therefore there
// is no markup. ch::MarkdownModel turns the document into a list of blocks and
// this page renders every one of them with textFormat: Text.PlainText. Nothing
// here uses Text.MarkdownText, Text.StyledText, Text.RichText or Text.AutoText;
// those four are banned on every mobile surface.
//
// Three rules follow, and each is implemented below rather than assumed:
//   * an IMAGE is fetched only after file.resolvePath reports it inside the Dev
//     Session's repository root, and only ever through
//     viewerService.imageUrl() — an internal, provider-backed address that can
//     be answered from bytes this process already holds and from nothing else;
//   * a LINK is never followed on a tap. The destination is shown as plain text
//     and the user must confirm. http(s) may then open ViewerWebPage when the
//     build has QtWebView; otherwise nothing opens and the destination is left
//     on screen as text;
//   * NOTHING is ever handed to QDesktopServices or any OS handler. A remote
//     file:// or internal URL leaving this app is precisely the SPEC 7.4 leak
//     this page exists to not have.
Item {
    id: root

    property string remotePath: ""
    property url paneUrl
    property string repoRoot: ""
    property string paneId: ""
    signal openRequested(string path)
    signal titleRequested(string title)

    readonly property var service: (typeof viewerService !== "undefined") ? viewerService : null
    readonly property var capabilities: (typeof mobile !== "undefined" && mobile.capabilities)
                                        ? mobile.capabilities : null

    property bool loading: false
    property string errorText: ""
    property bool truncated: false
    property string requestedPath: ""

    // The directory the document lives in, which every relative image path is
    // resolved against. Derived here and nowhere else.
    readonly property string documentDir: {
        const i = root.remotePath.lastIndexOf("/");
        return i <= 0 ? "/" : root.remotePath.substring(0, i);
    }

    // Image bookkeeping, keyed by the path as WRITTEN in the document.
    //   pending  — resolvePath asked, answer outstanding
    //   resolved — absolute remote path, inside the root, safe to read
    //   refused  — why it will not be fetched (shown as alt text instead)
    property var imagePending: ({})
    property var imageResolved: ({})
    property var imageRefused: ({})
    property var imageSources: ({})

    // ch::MarkdownModel, minted by the service and parented to this page (see
    // MobileViewerService::createMarkdownModel). Not a declarative type because
    // ch_mobile is a plain static library rather than a QML module; parenting
    // keeps ownership in C++, so the engine cannot collect a model the ListView
    // below is still bound to.
    readonly property var blocks: root.service
                                  ? root.service.createMarkdownModel(root) : null

    // Hand back the bytes of every image THIS document had fetched. The cache is
    // LRU-bounded so this is not a leak, but a document with twenty images
    // should not keep holding twenty buffers once it is no longer on screen —
    // the same discipline ViewerImagePage applies to its single image.
    function forgetImages() {
        if (!root.service)
            return;
        for (const key in root.imageResolved)
            root.service.forgetImage(root.imageResolved[key]);
    }

    function reload() {
        root.forgetImages();
        root.requestedPath = "";
        root.errorText = "";
        root.truncated = false;
        root.loading = false;
        root.imagePending = ({});
        root.imageResolved = ({});
        root.imageRefused = ({});
        root.imageSources = ({});
        if (root.blocks)
            root.blocks.clear();
        if (root.remotePath.length === 0 || !root.service)
            return;
        root.loading = true;
        root.requestedPath = root.remotePath;
        root.service.readFile(root.remotePath);
    }

    // ONE request for the first target, not two: a page is built by assigning its
    // initial properties and only then running Component.onCompleted, so the
    // host's `remotePath` fires the change handler BEFORE completion and an
    // unconditional completion handler asked the server for the same thing twice
    // on every pane open. `started` keeps the change handler inert until the
    // object is fully built, so exactly one path issues the request.
    property bool started: false
    onRemotePathChanged: if (root.started) root.reload()
    Component.onCompleted: {
        root.started = true;
        root.reload();
    }
    Component.onDestruction: root.forgetImages()

    // Ask whether one document-relative image path lands inside the repository
    // root. Called once per distinct path; the answer is cached in the maps
    // above, because a document may reference the same badge fifty times.
    function requestImage(writtenPath) {
        if (writtenPath.length === 0 || !root.service)
            return;
        if (root.imageSources[writtenPath] !== undefined
                || root.imagePending[writtenPath] !== undefined
                || root.imageRefused[writtenPath] !== undefined)
            return;
        const absolute = root.documentDir === "/"
                       ? "/" + writtenPath
                       : root.documentDir + "/" + writtenPath;
        const pending = root.imagePending;
        pending[writtenPath] = absolute;
        root.imagePending = pending;
        // The base is the SESSION's repository root, not the document's
        // directory: "inside the project" is a property of the session (SPEC 9),
        // and a document deep in a tree must not be able to widen it.
        root.service.resolvePath(absolute, root.repoRoot);
    }

    function refuseImage(writtenPath, reason) {
        const refused = root.imageRefused;
        refused[writtenPath] = reason;
        root.imageRefused = refused;
        const pending = root.imagePending;
        delete pending[writtenPath];
        root.imagePending = pending;
    }

    // The written path whose pending resolve/read `absolutePath` belongs to, or
    // "" when this page asked nothing about it.
    function pendingKeyFor(absolutePath) {
        const pending = root.imagePending;
        for (const key in pending) {
            if (pending[key] === absolutePath)
                return key;
        }
        return "";
    }

    function resolvedKeyFor(absolutePath) {
        const resolved = root.imageResolved;
        for (const key in resolved) {
            if (resolved[key] === absolutePath)
                return key;
        }
        return "";
    }

    Connections {
        target: root.service

        function onFileRead(path, text, binary, revision, isTruncated) {
            if (path !== root.requestedPath)
                return;
            root.requestedPath = "";
            root.loading = false;
            root.truncated = isTruncated;
            if (binary) {
                root.errorText = qsTr("This file is not UTF-8 text, so it cannot be read as Markdown.");
                return;
            }
            if (root.blocks)
                root.blocks.setMarkdown(text);
        }

        function onFileError(path, message) {
            // Two conversations arrive on this signal: the document read, and
            // every resolvePath failure. Both are matched, neither is guessed.
            if (path === root.requestedPath) {
                root.requestedPath = "";
                root.loading = false;
                root.errorText = message;
                return;
            }
            const key = root.pendingKeyFor(path);
            if (key.length > 0)
                root.refuseImage(key, message);
        }

        function onPathResolved(path, resolvedPath, insideRepositoryRoot) {
            const key = root.pendingKeyFor(path);
            if (key.length === 0)
                return;
            if (!insideRepositoryRoot) {
                // NOT an error and not a permission failure: SPEC 9 allows a
                // user to open files outside the root. What is refused is a
                // DOCUMENT pointing this client at a path outside the project
                // and having it read automatically.
                root.refuseImage(key, qsTr("outside the project"));
                return;
            }
            const resolved = root.imageResolved;
            resolved[key] = resolvedPath;
            root.imageResolved = resolved;
            // Only now does a read happen, and only through the service.
            root.service.requestImage(resolvedPath);
        }

        function onImageReady(path, url) {
            const key = root.resolvedKeyFor(path);
            if (key.length === 0)
                return;
            const sources = root.imageSources;
            sources[key] = url;
            root.imageSources = sources;
            const pending = root.imagePending;
            delete pending[key];
            root.imagePending = pending;
        }

        function onImageError(path, message) {
            const key = root.resolvedKeyFor(path);
            if (key.length > 0)
                root.refuseImage(key, message);
        }
    }

    // ---- link confirmation --------------------------------------------------

    property string pendingLink: ""

    function offerLink(target) {
        if (target.length === 0)
            return;
        root.pendingLink = target;
        linkSheet.open();
    }

    readonly property bool pendingLinkIsWeb: /^https?:\/\//i.test(root.pendingLink)

    Dialog {
        id: linkSheet
        objectName: "markdownLinkSheet"
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * MobileTheme.spacingLarge, 480)
        modal: true
        title: qsTr("Open this link?")
        standardButtons: root.pendingLinkIsWeb && root.capabilities && root.capabilities.hasWebView
                         ? (Dialog.Open | Dialog.Cancel) : Dialog.Cancel

        contentItem: Column {
            spacing: MobileTheme.spacing
            Text {
                width: linkSheet.availableWidth
                // The destination is server-controlled text and is displayed as
                // exactly that: no shortening that could hide a host, no
                // rendering as a link, no interpretation.
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                color: MobileTheme.text
                font.family: MobileTheme.monoFamily
                font.pixelSize: MobileTheme.fontSizeSmall
                text: root.pendingLink
            }
            Text {
                width: linkSheet.availableWidth
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: MobileTheme.textDim
                font.pixelSize: MobileTheme.fontSizeSmall
                text: !root.pendingLinkIsWeb
                      ? qsTr("This is not an http or https address, so nothing will be opened. The destination is shown above as text.")
                      : (root.capabilities && root.capabilities.hasWebView
                         ? qsTr("This will open inside CodeHarbor's own web view. No other application is involved.")
                         : qsTr("This build has no web view, so nothing will be opened. The destination is shown above as text."))
            }
        }

        onAccepted: {
            if (root.pendingLinkIsWeb && root.capabilities && root.capabilities.hasWebView)
                webLoader.open(root.pendingLink);
            root.pendingLink = "";
        }
        onRejected: root.pendingLink = ""
    }

    // The web view is loaded INSIDE this pane, keeping the single-pane invariant:
    // there is no second window, and closing it returns to the document.
    //
    // Referenced by FILE NAME rather than as a declarative ViewerWebPage type on
    // purpose: ViewerWebPage.qml is only added to the QML module when the
    // optional Qt6::WebView package was found, and a declarative type reference
    // would make THIS file fail to compile on a build without it — losing the
    // markdown viewer entirely to a capability it only optionally uses.
    Loader {
        id: webLoader
        anchors.fill: parent
        z: 10
        active: false
        property string target: ""
        function open(url) {
            webLoader.target = url;
            webLoader.active = true;
        }
        source: "ViewerWebPage.qml"
        onLoaded: {
            item.webUrl = webLoader.target;
            item.closeRequested.connect(function() { webLoader.active = false; });
        }
    }

    // ---- rendering ----------------------------------------------------------

    Rectangle {
        anchors.fill: parent
        color: MobileTheme.surface
    }

    // One block's inline text, rendered as plain-text RUNS.
    //
    // A run is split into words and laid out by a Flow, which is what keeps
    // wrapping working: Qt Quick's Text cannot style part of its own string
    // without rich text, and rich text is banned here, so several Text items are
    // the only honest way to show a bold word inside a wrapped paragraph.
    component SpanText: Flow {
        id: spanFlow
        property string body: ""
        property var spans: []
        property real size: MobileTheme.fontSizeBody
        property color textColor: MobileTheme.text
        property bool bold: false
        spacing: 0

        // [{ text, strong, emphasis, code, target }], covering `body` exactly
        // once and in order. Gaps between spans are plain runs.
        readonly property var runs: {
            const out = [];
            let at = 0;
            for (let i = 0; i < spanFlow.spans.length; ++i) {
                const span = spanFlow.spans[i];
                if (span.start > at)
                    out.push({ text: spanFlow.body.substring(at, span.start),
                               strong: false, emphasis: false, code: false, target: "" });
                out.push({ text: spanFlow.body.substring(span.start, span.start + span.length),
                           strong: span.strong === true,
                           emphasis: span.emphasis === true,
                           code: span.code === true,
                           target: span.link === true ? String(span.target) : "" });
                at = span.start + span.length;
            }
            if (at < spanFlow.body.length)
                out.push({ text: spanFlow.body.substring(at),
                           strong: false, emphasis: false, code: false, target: "" });
            return out;
        }

        // Words, so the Flow can break lines. A run of whitespace is APPENDED to
        // the word before it rather than becoming an item of its own: a lone
        // space item is a wrappable child, so the Flow could put it at the start
        // of a line and indent that line by a space.
        //
        // Two exceptions, both to keep a space from inheriting a style it does
        // not have: whitespace containing a newline stays its own item because
        // that one has to FORCE a break, and whitespace after a link or a code
        // run stays its own PLAIN item rather than being underlined or set in the
        // monospace face.
        readonly property var words: {
            const out = [];
            for (let i = 0; i < spanFlow.runs.length; ++i) {
                const run = spanFlow.runs[i];
                const pieces = run.text.split(/(\s+)/);
                for (let j = 0; j < pieces.length; ++j) {
                    if (pieces[j].length === 0)
                        continue;
                    if (/^\s+$/.test(pieces[j]) && pieces[j].indexOf("\n") < 0) {
                        const prev = out.length > 0 ? out[out.length - 1] : null;
                        if (prev && !prev.newline && prev.target.length === 0
                                && !prev.code) {
                            prev.text += pieces[j];
                            continue;
                        }
                        out.push({ text: pieces[j], strong: false,
                                   emphasis: false, code: false, target: "",
                                   newline: false });
                        continue;
                    }
                    out.push({ text: pieces[j], strong: run.strong,
                               emphasis: run.emphasis, code: run.code,
                               target: run.target,
                               newline: pieces[j].indexOf("\n") >= 0 });
                }
            }
            return out;
        }

        Repeater {
            model: spanFlow.words
            delegate: Item {
                required property var modelData
                // A run containing a newline forces the Flow onto the next line
                // by occupying the rest of this one.
                width: modelData.newline ? spanFlow.width : label.implicitWidth
                height: label.implicitHeight
                Text {
                    id: label
                    textFormat: Text.PlainText
                    // Whitespace preserved: the model joined a paragraph's lines
                    // with '\n' and that is the only layout information the
                    // author gave us.
                    text: modelData.newline ? "" : modelData.text
                    color: modelData.target.length > 0 ? MobileTheme.accent : spanFlow.textColor
                    font.pixelSize: spanFlow.size
                    font.bold: spanFlow.bold || modelData.strong
                    font.italic: modelData.emphasis
                    font.underline: modelData.target.length > 0
                    // The application font, not a theme role: MobileTheme names
                    // only the MONOSPACE family, and the proportional face is
                    // whatever the platform gives Qt Quick.
                    font.family: modelData.code ? MobileTheme.monoFamily
                                                : Qt.application.font.family
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: modelData.target.length > 0
                    onClicked: root.offerLink(modelData.target)
                }
            }
        }
    }

    Column {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            width: parent.width
            height: visible ? Math.max(MobileTheme.touchTarget, docBanner.implicitHeight + 2 * MobileTheme.spacing) : 0
            visible: root.errorText.length > 0 || root.truncated
            color: root.errorText.length > 0 ? MobileTheme.errorSurface() : MobileTheme.warningSurface()
            Text {
                id: docBanner
                anchors.fill: parent
                anchors.margins: MobileTheme.spacing
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                color: MobileTheme.text
                font.pixelSize: MobileTheme.fontSizeSmall
                text: root.errorText.length > 0
                      ? root.errorText
                      : qsTr("Only the first 8 MiB of this document was read.")
            }
        }

        ListView {
            id: blockList
            objectName: "markdownBlocks"
            width: parent.width
            height: parent.height - y
            clip: true
            model: root.blocks
            spacing: MobileTheme.spacing
            cacheBuffer: height * 2
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: blockDelegate
                required property string blockKind
                required property string text
                required property int level
                required property string language
                required property string imagePath
                required property bool ordered
                required property var checked
                required property var spans

                width: blockList.width
                implicitHeight: body.implicitHeight
                height: implicitHeight

                Loader {
                    id: body
                    width: parent.width
                    sourceComponent: {
                        switch (blockDelegate.blockKind) {
                        case "heading": return headingBlock;
                        case "code": return codeBlock;
                        case "listItem": return listBlock;
                        case "quote": return quoteBlock;
                        case "rule": return ruleBlock;
                        case "image": return imageBlock;
                        default: return paragraphBlock;
                        }
                    }
                }

                Component {
                    id: paragraphBlock
                    SpanText {
                        width: blockDelegate.width - 2 * MobileTheme.spacing
                        x: MobileTheme.spacing
                        body: blockDelegate.text
                        spans: blockDelegate.spans
                    }
                }

                Component {
                    id: headingBlock
                    SpanText {
                        width: blockDelegate.width - 2 * MobileTheme.spacing
                        x: MobileTheme.spacing
                        body: blockDelegate.text
                        spans: blockDelegate.spans
                        bold: true
                        // Six levels, largest first, floored at body size: a
                        // deeply nested heading must still be legible.
                        size: Math.max(MobileTheme.fontSizeBody,
                                       MobileTheme.fontSizeTitle + 6 - 2 * blockDelegate.level)
                    }
                }

                Component {
                    id: codeBlock
                    Rectangle {
                        width: blockDelegate.width
                        implicitHeight: codeColumn.implicitHeight + 2 * MobileTheme.spacing
                        height: implicitHeight
                        color: MobileTheme.surfaceSunken
                        radius: MobileTheme.radiusSmall
                        Column {
                            id: codeColumn
                            x: MobileTheme.spacing
                            y: MobileTheme.spacing
                            width: parent.width - 2 * MobileTheme.spacing
                            spacing: MobileTheme.spacingSmall
                            Text {
                                visible: blockDelegate.language.length > 0
                                textFormat: Text.PlainText
                                color: MobileTheme.textFaint
                                font.pixelSize: MobileTheme.fontSizeSmall
                                // A LABEL. Nothing is highlighted, parsed or run
                                // on the strength of this word.
                                text: blockDelegate.language
                            }
                            Text {
                                width: parent.width
                                textFormat: Text.PlainText
                                wrapMode: Text.WrapAnywhere
                                color: MobileTheme.text
                                font.family: MobileTheme.monoFamily
                                font.pixelSize: MobileTheme.fontSizeSmall
                                text: blockDelegate.text
                            }
                        }
                    }
                }

                Component {
                    id: listBlock
                    Row {
                        width: blockDelegate.width
                        spacing: MobileTheme.spacing
                        // Indent by nesting level, so a nested list reads as one.
                        leftPadding: MobileTheme.spacing
                                     + blockDelegate.level * MobileTheme.spacingLarge
                        Text {
                            textFormat: Text.PlainText
                            color: MobileTheme.textDim
                            font.pixelSize: MobileTheme.fontSizeBody
                            // Three states for `checked`: undefined means the
                            // item carries no task marker at all.
                            //
                            // An UNORDERED item gets the bullet, which is the
                            // convention every reader already knows; an ordered
                            // one gets a dash because ch::MarkdownModel carries
                            // no ordinal (see OrderedRole), so there is no number
                            // to print and a bullet would claim it was a bulleted
                            // list. The two were the wrong way round.
                            text: blockDelegate.checked === undefined
                                  ? (blockDelegate.ordered ? "–" : "•")
                                  : (blockDelegate.checked ? "☑" : "☐")
                        }
                        SpanText {
                            width: blockDelegate.width - 4 * MobileTheme.spacing
                                   - blockDelegate.level * MobileTheme.spacingLarge
                            body: blockDelegate.text
                            spans: blockDelegate.spans
                        }
                    }
                }

                Component {
                    id: quoteBlock
                    Row {
                        width: blockDelegate.width
                        spacing: MobileTheme.spacing
                        leftPadding: MobileTheme.spacing
                        Rectangle {
                            width: 3
                            height: quoteText.implicitHeight
                            color: MobileTheme.borderSubtle
                        }
                        SpanText {
                            id: quoteText
                            width: blockDelegate.width - 4 * MobileTheme.spacing
                            body: blockDelegate.text
                            spans: blockDelegate.spans
                            textColor: MobileTheme.textDim
                        }
                    }
                }

                Component {
                    id: ruleBlock
                    Item {
                        width: blockDelegate.width
                        implicitHeight: MobileTheme.spacingLarge
                        height: implicitHeight
                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width - 2 * MobileTheme.spacingLarge
                            height: 1
                            color: MobileTheme.border
                        }
                    }
                }

                Component {
                    id: imageBlock
                    Item {
                        width: blockDelegate.width
                        implicitHeight: remoteImage.visible ? remoteImage.height
                                                            : altText.implicitHeight
                        height: implicitHeight

                        // An empty imagePath means ch::MarkdownModel refused the
                        // destination (absolute, scheme-bearing, or containing a
                        // ".." segment). Nothing is asked for and nothing is
                        // fetched; the alt text is the whole rendering.
                        Component.onCompleted: {
                            if (blockDelegate.imagePath.length > 0)
                                root.requestImage(blockDelegate.imagePath);
                        }

                        readonly property url resolvedSource:
                            blockDelegate.imagePath.length > 0
                            && root.imageSources[blockDelegate.imagePath] !== undefined
                            ? root.imageSources[blockDelegate.imagePath] : ""

                        Image {
                            id: remoteImage
                            x: MobileTheme.spacing
                            // Fit to the pane, but never UPSCALE: a 16-pixel
                            // badge blown up to the full pane width is a blurry
                            // lie about what the file contains. Qt reports the
                            // source's natural size as implicitWidth regardless
                            // of this binding, so there is no loop here.
                            width: implicitWidth > 0
                                   ? Math.min(implicitWidth,
                                              parent.width - 2 * MobileTheme.spacing)
                                   : parent.width - 2 * MobileTheme.spacing
                            fillMode: Image.PreserveAspectFit
                            // Bytes come from ch::MobileImageProvider, which can
                            // only answer from the cache the service filled.
                            // `cache: false` so a re-read really re-requests.
                            asynchronous: true
                            cache: false
                            source: parent.resolvedSource
                            visible: status === Image.Ready
                            height: status === Image.Ready
                                    ? width * (implicitHeight / Math.max(1, implicitWidth))
                                    : 0
                        }

                        Text {
                            id: altText
                            x: MobileTheme.spacing
                            width: parent.width - 2 * MobileTheme.spacing
                            visible: !remoteImage.visible
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            color: MobileTheme.textDim
                            font.pixelSize: MobileTheme.fontSizeSmall
                            font.italic: true
                            text: {
                                const alt = blockDelegate.text.length > 0
                                          ? blockDelegate.text : qsTr("image");
                                if (blockDelegate.imagePath.length === 0)
                                    return alt + " " + qsTr("(not shown: the document points outside this project)");
                                const refusal = root.imageRefused[blockDelegate.imagePath];
                                if (refusal !== undefined)
                                    return alt + " (" + refusal + ")";
                                return alt;
                            }
                        }
                    }
                }
            }
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: root.loading
        visible: root.loading
    }
}
