import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// First-use host-key approval (SPEC 12.1). The one decision in this client that
// nobody but the user may make, so it is a modal sheet with two explicit buttons
// and no default.
//
// WHAT REACHES HERE, AND WHAT DOES NOT. ch::SshConnectionPool asks its host-key
// callback ONLY for KnownHosts::Verdict::Unknown — a host with no trusted entry
// at all. A CHANGED key for a host that IS trusted is Verdict::Mismatch, which
// the pool refuses outright and reports through hostKeyMismatch(); there is
// deliberately no in-app button that can approve it, because an "approve" button
// on that path is exactly what a man-in-the-middle needs (see the known-hosts
// note in docs/SPEC.md §12.1). So this sheet never offers to accept a changed
// key.
//
// It still has to be able to SAY when something is wrong, though: a prompt for a
// host the trusted-host store already knows is not routine first use. It asks
// ch::MobileKeyStore::knownHostInfo() — which resolves the endpoint through
// SshConnectionPool::lookupHostFor(), so "[host]:port" and hashed and wildcard
// known_hosts entries are all handled by the authority rather than by a second
// spelling here — and when that says the host is already trusted, the sheet turns
// into a warning worded as a possible attack and the accept button goes away.
//
// NEVER DEFAULTS TO TRUST. There is no autoAccept and no timeout that accepts.
// The two buttons are the only way to answer, the reject button holds the initial
// focus, Escape and the Android back key are wired to reject(), and — because a
// key handler only ever fires when this sheet actually holds key focus — ANY
// close that reached no decision is turned into a refusal by onClosed below. A
// dismissal can therefore never become an acceptance, and it can never leave the
// handshake parked with nobody answering it either.
//
// SECURITY (SPEC 7.5): the host, the key type and the fingerprint are strings an
// UNVERIFIED peer supplied, and the decision has to be made on the exact
// characters. Every one of them is rendered with textFormat: Text.PlainText —
// Text.StyledText honours <font color>, and Text.AutoText opts into StyledText by
// itself, so either could restyle or hide part of a fingerprint the user is being
// asked to compare.
Dialog {
    id: root

    // ch::MobileAppController — hostKeyPrompt(host, fingerprint, keyType) in,
    // acceptHostKey(bool) out.
    property var controller: null
    // ch::MobileKeyStore, for the already-trusted check. Null degrades to "cannot
    // tell", which is reported as such rather than assumed safe.
    property var keyStoreRef: null
    // The port of the attempt in flight. The pool hands the callback the BARE
    // host, so the canonical known_hosts token needs the port put back.
    property int port: 22

    readonly property string host: root.promptHost
    property string promptHost: ""
    property string promptFingerprint: ""
    property string promptKeyType: ""

    // Result of the trusted-host lookup for this prompt, refreshed when the
    // prompt arrives — not as a live binding, because the store is read from disk
    // and the answer must be the one that was true when the question was asked.
    property var trustInfo: null
    readonly property bool previouslyTrusted: root.trustInfo
                                              && root.trustInfo.known === true
    readonly property bool trustUnknowable: root.keyStoreRef === null

    modal: true
    // The sheet takes key focus so the handlers below can receive Escape and the
    // Android back key at all: a Popup that does not want focus never sees a key
    // event.
    focus: true
    // Neither a tap outside nor Escape may dismiss this silently, so no automatic
    // close policy — the answer paths below are explicit.
    closePolicy: Popup.NoAutoClose
    width: Math.min(parent ? parent.width - 32 : 320, 460)
    title: root.previouslyTrusted ? qsTr("Host key does not match")
                                  : qsTr("Unrecognised server")

    Connections {
        target: root.controller
        ignoreUnknownSignals: true

        function onHostKeyPrompt(host, fingerprint, keyType) {
            root.promptHost = host
            root.promptFingerprint = fingerprint
            root.promptKeyType = keyType
            root.trustInfo = root.keyStoreRef
                             ? root.keyStoreRef.knownHostInfo(host, root.port)
                             : null
            root.open()
        }
    }

    // The decision is reported EXACTLY once. Both buttons, the key handlers and
    // the closed-without-an-answer fallback all funnel through here, and the
    // handshake on the other side of acceptHostKey() cannot survive being told
    // twice.
    property bool answered: false

    function answer(trusted) {
        if (root.answered)
            return
        root.answered = true
        var controllerRef = root.controller
        root.close()
        if (controllerRef)
            controllerRef.acceptHostKey(trusted)
    }

    function trust() { root.answer(true) }
    function reject() { root.answer(false) }

    // A fresh prompt is a fresh decision. Set here rather than in the Connections
    // handler so a sheet reopened by any route starts undecided.
    onOpened: root.answered = false
    // The safety net: a close that answered nothing is a refusal. This is what
    // makes "no dismissal can be an acceptance" true structurally instead of by
    // enumerating dismissal routes, and it is what stops a close from leaving
    // ch::SshConnectionPool waiting on a callback nobody will call. answer()
    // guards the double, so the ordinary button paths do not reach this.
    onClosed: root.answer(false)

    // Escape and the Android back key: refuse. Both are routed through the same
    // answer path as the buttons.
    //
    // The handlers live on the contentItem, not on the Dialog: a Popup is not an
    // Item, so `Keys` cannot attach to it and Qt says so at load time rather
    // than silently. The contentItem IS an Item, and `focus: true` here plus
    // `focus: true` on the Dialog is what puts it in line for key events. Qt maps
    // the Android back button to Qt::Key_Back, NOT to Escape, so
    // onEscapePressed alone would do nothing on the platform this client is for.
    contentItem: ColumnLayout {
        focus: true
        spacing: 12

        Keys.onEscapePressed: (event) => {
            root.reject()
            event.accepted = true
        }
        Keys.onBackPressed: (event) => {
            root.reject()
            event.accepted = true
        }

        // The danger case first, and unmissable: same host, different key.
        Frame {
            objectName: "mismatchWarning"
            Layout.fillWidth: true
            visible: root.previouslyTrusted
            background: Rectangle {
                color: "#5a1111"
                radius: 4
            }
            ColumnLayout {
                anchors.fill: parent
                spacing: 6
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: "#ffffff"
                    font.bold: true
                    text: qsTr("This server is already trusted with a DIFFERENT key.")
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: "#ffffff"
                    text: qsTr("Someone may be impersonating the server, or its "
                               + "key was replaced. CodeHarbor will not approve a "
                               + "changed key. If you know the key really did "
                               + "change, edit the trusted-host file deliberately, "
                               + "outside CodeHarbor.")
                }
                Label {
                    objectName: "knownFingerprintsLabel"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    color: "#ffffff"
                    font.family: "monospace"
                    font.pixelSize: 11
                    text: {
                        if (!root.trustInfo || !root.trustInfo.fingerprints)
                            return ""
                        var list = root.trustInfo.fingerprints
                        if (list.length === 0)
                            return ""
                        return qsTr("Already trusted: ") + list.join("\n")
                    }
                }
            }
        }

        Label {
            objectName: "hostKeyHeadline"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            // Server-supplied host name and key type: literal characters only.
            textFormat: Text.PlainText
            text: qsTr("%1 presented a %2 key that is not in the trusted-host file.")
                  .arg(root.promptHost)
                  .arg(root.promptKeyType)
        }

        Label {
            objectName: "hostKeyEndpoint"
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WrapAnywhere
            textFormat: Text.PlainText
            font.family: "monospace"
            font.pixelSize: 11
            // The exact token that will be written to the trusted-host file, so
            // there is no doubt which endpoint the decision covers — a key
            // approved for [box]:2222 says nothing about box on port 22.
            text: root.trustInfo ? String(root.trustInfo.lookupHost) : ""
        }

        Label {
            objectName: "hostKeyFingerprint"
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            textFormat: Text.PlainText
            font.family: "monospace"
            font.pixelSize: 13
            // The whole decision rests on these characters matching what the
            // server's own `ssh-keygen -lf /etc/ssh/ssh_host_*_key.pub` prints.
            text: root.promptFingerprint
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            font.pixelSize: 12
            visible: !root.previouslyTrusted
            text: qsTr("Compare this fingerprint with the server itself before you "
                       + "trust it. Trusting stores the key, and you will not be "
                       + "asked again.")
        }

        Label {
            objectName: "trustUnknowableLabel"
            Layout.fillWidth: true
            visible: root.trustUnknowable
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            font.pixelSize: 12
            text: qsTr("CodeHarbor could not check whether this server is already "
                       + "trusted, so treat an unexpected prompt as suspicious.")
        }
    }

    footer: DialogButtonBox {
        // Reject is listed first and holds the focus: this dialog must never be
        // dismissed into an acceptance by a stray tap or an Enter key.
        Button {
            objectName: "hostKeyRejectButton"
            text: qsTr("Reject")
            focus: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        Button {
            objectName: "hostKeyAcceptButton"
            text: qsTr("Trust and remember")
            // Absent, not merely disabled, for a host already trusted with another
            // key: there is no code path in ch::SshConnectionPool that would honour
            // it, so showing it would promise something the client refuses to do.
            visible: !root.previouslyTrusted
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
    }

    onAccepted: root.trust()
    onRejected: root.reject()
}
