#pragma once

#include <QObject>

QT_BEGIN_NAMESPACE
class QQuickWebEngineProfile;
QT_END_NAMESPACE

namespace ch {

class CodeharbordClient;
class InternalUrlMap;
class InternalUrlSchemeHandler;

// Owns the two Qt WebEngine security contexts required by SPEC 7.2/7.3: a
// sandboxed EXTERNAL profile for arbitrary http/https pages and a privileged
// INTERNAL profile for the application's own viewers.
//
//   * external — persistent cookies/localStorage, NO internal scheme handler,
//     NO WebChannel bridge. Arbitrary/authenticated sites live here and can
//     never reach remote files or the internal API.
//   * internal — installs the InternalUrlSchemeHandler (remote-file access via
//     codeharbor-internal://) and permits a WebChannel bridge.
//
// registerUrlScheme() MUST be called exactly once BEFORE QtWebEngineQuick is
// initialized; the instance methods are used afterwards to obtain the profiles.
class ViewerProfiles : public QObject {
    Q_OBJECT
public:
    // `client` is injected into the internal profile's scheme handler for remote
    // reads; not owned. May be null in tests that only assert isolation.
    explicit ViewerProfiles(CodeharbordClient *client = nullptr,
                            QObject *parent = nullptr);
    ~ViewerProfiles() override;

    // Register the codeharbor-internal scheme with WebEngine. Idempotent within
    // a process and safe to call before QtWebEngine init; a no-op afterwards.
    static void registerUrlScheme();

    // Lazily created; the same object is returned on repeated calls. These are
    // the QML profile type (QQuickWebEngineProfile) so they bind directly to
    // WebEngineView.profile from QML.
    QQuickWebEngineProfile *externalProfile();
    QQuickWebEngineProfile *internalProfile();

    // Isolation invariants (M3): the external profile must NOT carry the
    // internal scheme handler, the internal profile MUST. Reflect the current
    // state; creating the respective profile makes the answer meaningful.
    bool externalHasInternalScheme() const;
    bool internalHasInternalScheme() const;

private:
    CodeharbordClient *m_client;
    InternalUrlSchemeHandler *m_handler = nullptr;
    QQuickWebEngineProfile *m_external = nullptr;
    QQuickWebEngineProfile *m_internal = nullptr;
};

} // namespace ch
