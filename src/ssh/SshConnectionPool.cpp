#include "SshConnectionPool.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace ch {


QString SshConnectionPool::lookupHostFor(const QString& host, quint16 port)
{
    return port == 22 ? host : QStringLiteral("[%1]:%2").arg(host).arg(port);
}


bool SshConnectionPool::isWindowsNamedPipeAgentSocket(const QString& socket)
{
    return socket.startsWith(QStringLiteral("\\\\.\\pipe\\"),
                             Qt::CaseInsensitive);
}

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
                                      const QString& user,
                                      const QString& identityFile)
{
    Q_UNUSED(host);
    Q_UNUSED(port);
    Q_UNUSED(user);
    Q_UNUSED(identityFile);
    emit errorOccurred(
        QStringLiteral("SSH support unavailable: built without libssh"));
    return false;
}

void SshConnectionPool::disconnectFromHost()
{
    // Nothing to tear down; remain in NotAvailable.
}

#else

namespace {

QString resolveIdentityFilePath(QString identityFile)
{
    identityFile = identityFile.trimmed();
    if (identityFile == QLatin1String("~"))
        return QDir::homePath();
    if (identityFile.startsWith(QLatin1String("~/"))
        || identityFile.startsWith(QLatin1String("~\\"))) {
        return QDir::home().filePath(identityFile.sliced(2));
    }
    return QDir::cleanPath(identityFile);
}

bool usesUnsupportedWindowsAgent()
{
#ifdef Q_OS_WIN
    constexpr bool onWindows = true;
#else
    constexpr bool onWindows = false;
#endif
    // Windows' built-in OpenSSH agent exposes a named pipe, whereas libssh's
    // agent implementation opens SSH_AUTH_SOCK as an AF_UNIX socket. Trying
    // the pipe poisons libssh's public-key auth state before local key fallback
    // can run. AF_UNIX sockets remain supported on current Windows, so bypass
    // only the named-pipe spelling.
    return onWindows && SshConnectionPool::isWindowsNamedPipeAgentSocket(
        qEnvironmentVariable("SSH_AUTH_SOCK"));
}

int declineLibsshPassphrase(const char*, char*, size_t, int, int, void*)
{
    // CodeHarbor owns passphrase prompts. libssh must never fall back to a
    // controlling-terminal prompt inside the desktop client.
    return -1;
}

QStringList identityFileCandidates(ssh_session session,
                                   const QString& profileIdentityFile)
{
    QStringList candidates;
    const auto add = [&candidates](const QString& file) {
        if (!file.isEmpty() && !candidates.contains(file))
            candidates.append(file);
    };

    add(profileIdentityFile);

    // The public API exposes the first identity parsed from ~/.ssh/config.
    // Keep it after the explicitly saved profile value, which deliberately has
    // precedence over broader OpenSSH defaults.
    char* configuredIdentity = nullptr;
    if (ssh_options_get(session, SSH_OPTIONS_IDENTITY, &configuredIdentity)
        == SSH_OK) {
        add(QFile::decodeName(configuredIdentity));
        ssh_string_free_char(configuredIdentity);
    }

    // With a profile or config identity, stop there: each rejected key spends
    // one server authentication attempt. Scan defaults only when neither
    // source named a key, leaving room for a later passphrase retry.
    if (candidates.isEmpty()) {
        const QDir sshDirectory(
            QDir::home().filePath(QStringLiteral(".ssh")));
        for (const QString& fileName : {QStringLiteral("id_ed25519"),
                                        QStringLiteral("id_ecdsa"),
                                        QStringLiteral("id_rsa")}) {
            const QString candidate = sshDirectory.filePath(fileName);
            if (QFileInfo::exists(candidate))
                add(candidate);
        }
    }
    return candidates;
}

bool authenticateIdentityFile(ssh_session session, const QString& identityFile,
                              const QString& passphrase)
{
    const QByteArray fileName = QFile::encodeName(identityFile);
    const QByteArray passphraseUtf8 = passphrase.toUtf8();
    ssh_key privateKey = nullptr;
    const int importResult = ssh_pki_import_privkey_file(
        fileName.constData(),
        passphrase.isEmpty() ? nullptr : passphraseUtf8.constData(),
        declineLibsshPassphrase, nullptr, &privateKey);
    if (importResult != SSH_OK || !privateKey)
        return false;

    const int authenticationResult =
        ssh_userauth_publickey(session, nullptr, privateKey);
    ssh_key_free(privateKey);
    return authenticationResult == SSH_AUTH_SUCCESS;
}

bool authenticateIdentityFiles(ssh_session session,
                               const QStringList& identityFiles,
                               const QString& passphrase)
{
    for (const QString& identityFile : identityFiles) {
        if (authenticateIdentityFile(session, identityFile, passphrase))
            return true;
    }
    return false;
}

} // namespace

bool SshConnectionPool::connectToHost(const QString& host, quint16 port,
                                      const QString& user,
                                      const QString& identityFile)
{
    disconnectFromHost();
    m_host = host;
    m_port = port;
    m_user = user;
    m_identityFile = resolveIdentityFilePath(identityFile);

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

    // libssh only learns IdentityFile, ProxyJump and the rest of the user's
    // OpenSSH configuration when asked to parse it. Set Host first so its
    // `Host` blocks match, then re-apply the explicitly saved user/port below:
    // profile values deliberately win over broad config defaults.
    const QString configPath =
        QDir::home().filePath(QStringLiteral(".ssh/config"));
    if (QFileInfo(configPath).isFile()) {
        const QByteArray configUtf8 = QFile::encodeName(configPath);
        if (ssh_options_parse_config(m_session, configUtf8.constData())
            != SSH_OK) {
            emit errorOccurred(
                QStringLiteral("Could not parse SSH config %1: %2")
                    .arg(configPath, QString::fromUtf8(ssh_get_error(m_session))));
            closeSession();
            setState(State::Error);
            return false;
        }
    }

    ssh_options_set(m_session, SSH_OPTIONS_PORT, &portValue);
    ssh_options_set(m_session, SSH_OPTIONS_USER, userUtf8.constData());
    if (!m_identityFile.isEmpty()) {
        const QByteArray identityUtf8 = QFile::encodeName(m_identityFile);
        ssh_options_set(m_session, SSH_OPTIONS_IDENTITY,
                        identityUtf8.constData());
    }


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
        emit errorOccurred(authenticationFailure());
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
    // Channels are attached to the session; ssh_free() would free them too, so
    // free them explicitly FIRST to keep a clear ownership boundary and avoid a
    // double-free/UAF if a caller still holds a (now-invalid) handle.
    for (ssh_channel channel : m_channels) {
        if (!channel)
            continue;
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
    m_channels.clear();
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

    // Non-default ports are stored OpenSSH-style as "[host]:port"; match that
    // form so a ported entry is found (and persisted) correctly. The SAME token
    // pinned the algorithm list in connectToHost().
    const QString lookupHost = lookupHostFor(host, m_port);
    switch (m_knownHosts.verify(lookupHost, keyType, keyBlob)) {
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
            m_knownHosts.add(lookupHost, keyType, keyBlob);
            return true;
        }
        return false;
    }
    return false;
}

bool SshConnectionPool::authenticate(const QString& user)
{
    // ssh_userauth_list() is only defined after a "none" request. It is not
    // enough to query it after an auto-key failure: some libssh builds then
    // report no methods at all, silently skipping the passphrase callback.
    if (ssh_userauth_none(m_session, nullptr) == SSH_AUTH_SUCCESS)
        return true;
    const int methods = ssh_userauth_list(m_session, nullptr);

    // Windows' built-in OpenSSH agent is a named pipe, but libssh expects an
    // AF_UNIX socket. Avoid every libssh auto-auth call in that case: it
    // retries the agent internally and leaves public-key fallback unusable.
    const bool unsupportedWindowsAgent = usesUnsupportedWindowsAgent();
    const QStringList identityFiles =
        identityFileCandidates(m_session, m_identityFile);
    if (!unsupportedWindowsAgent
        && ssh_userauth_agent(m_session, nullptr) == SSH_AUTH_SUCCESS) {
        return true;
    }

    if (unsupportedWindowsAgent) {
        if (authenticateIdentityFiles(m_session, identityFiles, QString()))
            return true;
    } else if (ssh_userauth_publickey_auto(m_session, nullptr, nullptr)
               == SSH_AUTH_SUCCESS) {
        return true;
    }

    // A passphrase is used ONLY for public-key authentication: falling through
    // to ssh_userauth_password() with it would disclose a local key secret to
    // the remote server and makes the two credential classes indistinguishable.
    if (m_credentialCallback && (methods & SSH_AUTH_METHOD_PUBLICKEY)
        && (!unsupportedWindowsAgent || !identityFiles.isEmpty())) {
        CredentialReply passphrase =
            m_credentialCallback(user, CredentialKind::KeyPassphrase);
        if (passphrase.promptRequested)
            return false;
        if (!passphrase.secret.isEmpty()) {
            if (unsupportedWindowsAgent) {
                if (authenticateIdentityFiles(m_session, identityFiles,
                                              passphrase.secret)) {
                    return true;
                }
            } else {
                const QByteArray secretUtf8 = passphrase.secret.toUtf8();
                if (ssh_userauth_publickey_auto(m_session, nullptr,
                                                secretUtf8.constData())
                    == SSH_AUTH_SUCCESS) {
                    return true;
                }
            }
        }
    }

    // 4. Password authentication is an independent, opt-in credential. A GUI
    // that is parked on this request must not be overwritten by another prompt.
    if (m_credentialCallback && (methods & SSH_AUTH_METHOD_PASSWORD)) {
        CredentialReply password =
            m_credentialCallback(user, CredentialKind::Password);
        if (password.promptRequested)
            return false;
        if (!password.secret.isEmpty()) {
            const QByteArray secretUtf8 = password.secret.toUtf8();
            if (ssh_userauth_password(m_session, nullptr,
                                      secretUtf8.constData())
                == SSH_AUTH_SUCCESS) {
                return true;
            }
        }
    }
    return false;
}

QString SshConnectionPool::authenticationFailure() const
{
    QStringList details;
    details << QStringLiteral("Authentication failed.");
    if (usesUnsupportedWindowsAgent()) {
        details << QStringLiteral(
            "Windows OpenSSH's named-pipe ssh-agent cannot be used by this "
            "libssh build. Open Server… and set Private key file to a local key; "
            "CodeHarbor will ask for its passphrase when needed.");
    } else if (qEnvironmentVariableIsEmpty("SSH_AUTH_SOCK")) {
        details << QStringLiteral(
            "SSH_AUTH_SOCK is not available to this CodeHarbor process, so "
            "ssh-agent keys cannot be used.");
    } else {
        details << QStringLiteral(
            "ssh-agent was available to CodeHarbor but did not authenticate "
            "a key accepted by the server.");
    }

    if (m_identityFile.isEmpty()) {
        if (usesUnsupportedWindowsAgent()) {
            details << QStringLiteral(
                "No private key file is configured for this server profile.");
        } else {
            details << QStringLiteral(
                "No private key file is configured for this server profile; "
                "CodeHarbor also tried ~/.ssh/config and libssh defaults. Open "
                "Server… and set Private key file, or launch CodeHarbor from an "
                "environment that exports SSH_AUTH_SOCK.");
        }
    } else if (!QFileInfo(m_identityFile).isFile()) {
        details << QStringLiteral("Private key file does not exist: %1.")
                       .arg(m_identityFile);
    } else {
        details << QStringLiteral("Private key file was tried: %1.")
                       .arg(m_identityFile);
    }

    const QString libsshError =
        m_session ? QString::fromUtf8(ssh_get_error(m_session)).trimmed()
                  : QString();
    if (!libsshError.isEmpty())
        details << QStringLiteral("libssh: %1.").arg(libsshError);
    return details.join(QLatin1Char(' '));
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

    // Terminal channels want a PTY; the caller drives shell/exec and I/O. A
    // failed PTY request must not masquerade as a usable channel.
    if (kind == ChannelKind::Pty
        && ssh_channel_request_pty(channel) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return nullptr;
    }

    // The pool owns the channel: track it so closeSession() frees it before the
    // session, preventing a double-free through ssh_free()'s channel teardown.
    m_channels.append(channel);
    return channel;
}

#endif // CH_HAVE_LIBSSH

} // namespace ch
