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
    // secure context, and CORS-enabled so the privileged viewers can fetch
    // internal-scheme subresources. LocalAccessAllowed is deliberately NOT set:
    // everything is served via CodeharbordClient/readFile, never file://, so the
    // internal origin must not be granted client file:// reach (SPEC 2.4/7).
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme
                    | QWebEngineUrlScheme::CorsEnabled);
    QWebEngineUrlScheme::registerScheme(scheme);
}

QQuickWebEngineProfile *ViewerProfiles::externalProfile()
{
    if (!m_external) {
        // A named (persistent) profile persists cookies and local storage to
        // disk. It deliberately gets NO internal scheme handler and NO
        // WebChannel bridge — arbitrary sites stay sandboxed (SPEC 7.3, the
        // separate-profiles requirement; 7.2 is what those sites ARE).
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
        // opaque content and needs no on-disk persistence.
        //
        // Constructed through the storage-name overload with an EMPTY name
        // rather than the default ctor. Both build the same off-the-record
        // ProfileAdapter, but QQuickWebEngineProfile(QObject *) additionally
        // emits, from 6.9 on, "Please use WebEngineProfilePrototype for profile
        // creation ... will be deprecated in the future releases". An empty
        // storage name is precisely what WebEngineProfilePrototype{} feeds its
        // adapter when no storageName is set, so this IS the prototype's
        // off-the-record path — and the prototype type itself
        // (QQuickWebEngineProfilePrototype) is QML-only private API whose header
        // is not installed, so C++ cannot use it directly.
        m_internal = new QQuickWebEngineProfile(QString(), this);
        m_internal->installUrlSchemeHandler(internalSchemeName(),
                                            internalSchemeHandler());
    }
    return m_internal;
}

InternalUrlSchemeHandler *ViewerProfiles::internalSchemeHandler()
{
    if (!m_handler)
        m_handler = new InternalUrlSchemeHandler(m_client, nullptr, this);
    return m_handler;
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
