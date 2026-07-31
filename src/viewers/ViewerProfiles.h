#pragma once

#include <QObject>
#include <QPointer>

// Full definition (not a forward declaration): m_client is a QPointer, whose
// QObject static_cast needs the complete CodeharbordClient (a QObject) type.
#include "CodeharbordClient.h"

QT_BEGIN_NAMESPACE
class QQuickWebEngineProfile;
QT_END_NAMESPACE

namespace ch {

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
    // QPointer, not a raw pointer: the client is owned by the caller and can be
    // destroyed before this object is (the internal profile — and with it the
    // scheme handler this pointer is handed to — is created LAZILY, so the
    // pointer may not be read until long after it was passed in). A raw pointer
    // would dangle and be adopted by the handler; QPointer self-clears, and the
    // handler is simply constructed clientless and fails its reads honestly.
    QPointer<CodeharbordClient> m_client;
    InternalUrlSchemeHandler *m_handler = nullptr;
    QQuickWebEngineProfile *m_external = nullptr;
    QQuickWebEngineProfile *m_internal = nullptr;
};

} // namespace ch
