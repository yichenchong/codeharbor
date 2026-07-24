#include "ViewerProfiles.h"

#include "InternalUrlSchemeHandler.h"

#include <QByteArray>
#include <QWebEngineUrlScheme>
#include <QtWebEngineQuick/QQuickWebEngineProfile>

namespace ch {

namespace {
QByteArray internalSchemeName()
{
    return QByteArrayLiteral("codeharbor-internal");
}
} // namespace

ViewerProfiles::ViewerProfiles(CodeharbordClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
}

ViewerProfiles::~ViewerProfiles() = default;

void ViewerProfiles::registerUrlScheme()
{
    // Registering the same scheme twice logs a warning and is rejected by
    // WebEngine; guard so callers can invoke this unconditionally at startup.
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    QWebEngineUrlScheme scheme(internalSchemeName());
    // Host syntax: codeharbor-internal://file/<opaque-id> (authority = "file").
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    scheme.setDefaultPort(QWebEngineUrlScheme::PortUnspecified);
    // Treated as a secure origin (HTTPS-like) so pages served from it run in a
    // secure context; allowed to interoperate with local content and to satisfy
    // CORS/fetch from the privileged viewers.
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme
                    | QWebEngineUrlScheme::LocalAccessAllowed
                    | QWebEngineUrlScheme::CorsEnabled);
    QWebEngineUrlScheme::registerScheme(scheme);
}

QQuickWebEngineProfile *ViewerProfiles::externalProfile()
{
    if (!m_external) {
        // A named (persistent) profile persists cookies and local storage to
        // disk. It deliberately gets NO internal scheme handler and NO
        // WebChannel bridge — arbitrary sites stay sandboxed (SPEC 7.2).
        m_external =
            new QQuickWebEngineProfile(QStringLiteral("codeharbor-external"), this);
        m_external->setPersistentCookiesPolicy(
            QQuickWebEngineProfile::ForcePersistentCookies);
        m_external->setHttpCacheType(QQuickWebEngineProfile::DiskHttpCache);
    }
    return m_external;
}

QQuickWebEngineProfile *ViewerProfiles::internalProfile()
{
    if (!m_internal) {
        // Off-the-record: the privileged profile serves ephemeral, app-minted
        // opaque content and needs no on-disk persistence. The default ctor
        // yields an off-the-record profile.
        m_internal = new QQuickWebEngineProfile(this);
        if (!m_handler)
            m_handler = new InternalUrlSchemeHandler(m_client, nullptr, this);
        m_internal->installUrlSchemeHandler(internalSchemeName(), m_handler);
    }
    return m_internal;
}

bool ViewerProfiles::externalHasInternalScheme() const
{
    return m_external
           && m_external->urlSchemeHandler(internalSchemeName()) != nullptr;
}

bool ViewerProfiles::internalHasInternalScheme() const
{
    return m_internal
           && m_internal->urlSchemeHandler(internalSchemeName()) != nullptr;
}

} // namespace ch
