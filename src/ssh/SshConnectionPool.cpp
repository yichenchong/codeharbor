#include "SshConnectionPool.h"

namespace ch {

SshConnectionPool::SshConnectionPool(QObject* parent)
    : QObject(parent)
{
#if !CH_HAVE_LIBSSH
    m_state = State::NotAvailable;
#endif
}

SshConnectionPool::~SshConnectionPool()
{
    disconnectFromHost();
}

bool SshConnectionPool::libsshAvailable()
{
#if CH_HAVE_LIBSSH
    return true;
#else
    return false;
#endif
}

void SshConnectionPool::setKnownHosts(const KnownHosts& hosts)
{
    m_knownHosts = hosts;
}

const KnownHosts& SshConnectionPool::knownHosts() const
{
    return m_knownHosts;
}

void SshConnectionPool::setHostKeyCallback(HostKeyCallback callback)
{
    m_hostKeyCallback = std::move(callback);
}

void SshConnectionPool::setCredentialCallback(CredentialCallback callback)
{
    m_credentialCallback = std::move(callback);
}

SshConnectionPool::State SshConnectionPool::state() const
{
    return m_state;
}

void SshConnectionPool::setState(State next)
{
    if (m_state == next)
        return;
    m_state = next;
    emit stateChanged(next);
}

#if !CH_HAVE_LIBSSH

bool SshConnectionPool::connectToHost(const QString& host, quint16 port,
                                      const QString& user)
{
    Q_UNUSED(host);
    Q_UNUSED(port);
    Q_UNUSED(user);
    setState(State::NotAvailable);
    emit errorOccurred(
        QStringLiteral("SSH support unavailable: built without libssh"));
    return false;
}

void SshConnectionPool::disconnectFromHost()
{
    // Nothing to tear down; remain in NotAvailable.
}

#else

bool SshConnectionPool::connectToHost(const QString& host, quint16 port,
                                      const QString& user)
{
    disconnectFromHost();
    m_host = host;
    m_port = port;
    m_user = user;

    setState(State::Connecting);
    m_session = ssh_new();
    if (!m_session) {
        emit errorOccurred(QStringLiteral("ssh_new() failed"));
        setState(State::Error);
        return false;
    }

    const QByteArray hostUtf8 = host.toUtf8();
    const QByteArray userUtf8 = user.toUtf8();
    unsigned int portValue = port;
    ssh_options_set(m_session, SSH_OPTIONS_HOST, hostUtf8.constData());
    ssh_options_set(m_session, SSH_OPTIONS_PORT, &portValue);
    ssh_options_set(m_session, SSH_OPTIONS_USER, userUtf8.constData());

    if (ssh_connect(m_session) != SSH_OK) {
        emit errorOccurred(QString::fromUtf8(ssh_get_error(m_session)));
        closeSession();
        setState(State::Error);
        return false;
    }

    setState(State::HostKeyCheck);
    if (!verifyHostKey(host)) {
        closeSession();
        setState(State::Error);
        return false;
    }

    setState(State::Authenticating);
    if (!authenticate(user)) {
        emit errorOccurred(QStringLiteral("Authentication failed"));
        closeSession();
        setState(State::Error);
        return false;
    }

    setState(State::Connected);
    return true;
}

void SshConnectionPool::disconnectFromHost()
{
    closeSession();
    if (m_state != State::NotAvailable)
        setState(State::Disconnected);
}

void SshConnectionPool::closeSession()
{
    if (!m_session)
        return;
    if (ssh_is_connected(m_session))
        ssh_disconnect(m_session);
    ssh_free(m_session);
    m_session = nullptr;
}

bool SshConnectionPool::verifyHostKey(const QString& host)
{
    ssh_key serverKey = nullptr;
    if (ssh_get_server_publickey(m_session, &serverKey) != SSH_OK || !serverKey) {
        emit errorOccurred(QStringLiteral("Could not read server host key"));
        return false;
    }

    const char* typeName = ssh_key_type_to_char(ssh_key_type(serverKey));
    const QString keyType =
        typeName ? QString::fromUtf8(typeName) : QString();

    // libssh 0.11 exposes the host key as base64 (the OpenSSH known_hosts key
    // field); decode it to the raw blob KnownHosts stores and compares.
    char* b64Key = nullptr;
    if (ssh_pki_export_pubkey_base64(serverKey, &b64Key) != SSH_OK || !b64Key) {
        ssh_key_free(serverKey);
        emit errorOccurred(QStringLiteral("Could not export server host key"));
        return false;
    }
    const QByteArray keyBlob = QByteArray::fromBase64(QByteArray(b64Key));
    ssh_string_free_char(b64Key);
    ssh_key_free(serverKey);

    switch (m_knownHosts.verify(host, keyType, keyBlob)) {
    case KnownHosts::Verdict::Match:
        return true;
    case KnownHosts::Verdict::Mismatch:
        emit hostKeyMismatch(host);
        emit errorOccurred(
            QStringLiteral("Host key changed for %1 — refusing connection")
                .arg(host));
        return false;  // hard refusal (SPEC 12.1)
    case KnownHosts::Verdict::Unknown:
        if (!m_hostKeyCallback)
            return false;
        if (m_hostKeyCallback(host, keyType, keyBlob,
                              KnownHosts::Verdict::Unknown)
            == HostKeyDecision::Accept) {
            m_knownHosts.add(host, keyType, keyBlob);
            return true;
        }
        return false;
    }
    return false;
}

bool SshConnectionPool::authenticate(const QString& user)
{
    // 1. Prefer a running ssh-agent.
    if (ssh_userauth_agent(m_session, nullptr) == SSH_AUTH_SUCCESS)
        return true;

    // 2. Fall back to default identity files (may consult the agent/passphrase).
    if (ssh_userauth_publickey_auto(m_session, nullptr, nullptr)
        == SSH_AUTH_SUCCESS)
        return true;

    // 3. Last resort: a password/passphrase supplied by the OS credential store
    //    via the caller's callback. No secret is ever cached here (SPEC 12.1).
    if (m_credentialCallback) {
        const QString secret =
            m_credentialCallback(user, QStringLiteral("Password"));
        if (!secret.isEmpty()) {
            const QByteArray secretUtf8 = secret.toUtf8();
            const bool ok = ssh_userauth_password(m_session, nullptr,
                                                  secretUtf8.constData())
                            == SSH_AUTH_SUCCESS;
            if (ok)
                return true;
        }
    }
    return false;
}

ssh_channel SshConnectionPool::openChannel(ChannelKind kind)
{
    if (m_state != State::Connected || !m_session)
        return nullptr;

    ssh_channel channel = ssh_channel_new(m_session);
    if (!channel)
        return nullptr;

    if (ssh_channel_open_session(channel) != SSH_OK) {
        ssh_channel_free(channel);
        return nullptr;
    }

    // Terminal channels want a PTY; the caller drives shell/exec and I/O.
    if (kind == ChannelKind::Pty)
        ssh_channel_request_pty(channel);

    return channel;
}

#endif // CH_HAVE_LIBSSH

} // namespace ch
