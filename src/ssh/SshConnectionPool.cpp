#include "SshConnectionPool.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QScopeGuard>
// libssh version floor for this file and SshChannelDevice.cpp, audited symbol
// by symbol against the upstream release headers:
//   * Compile/link floor is 0.8.0, set by ONE symbol: ssh_get_server_publickey()
//     in verifyHostKey(). It first appears in include/libssh/libssh.h at tag
//     libssh-0.8.0 (which deprecates the older ssh_get_publickey()); every other
//     libssh function, enum and constant used here already exists at 0.7.0.
//   * The "-mlkem768x25519-sha256" value handed to SSH_OPTIONS_KEY_EXCHANGE
//     below needs the +,-,^ algorithm-list modifiers, added in 0.11.0. That call
//     only runs when ssh_version() reports 0.12.0, so an older runtime never
//     reaches it, but the mitigation itself is a 0.11.0-and-newer feature.
//   * Practical floor is higher than the API floor, because two advisories land
//     exactly on the calls made here: CVE-2023-6004 (command injection through a
//     ProxyCommand built from config, fixed in 0.10.6 — connectToHost() parses
//     the user's ~/.ssh/config) and CVE-2025-5351 (double free in the public-key
//     export path used by ssh_pki_export_pubkey_base64(); affects 0.10.0 and
//     newer built against OpenSSL 3, fixed in 0.11.2). 0.12.0 is excluded
//     outright — see hasBrokenHybridKex().
#if CH_HAVE_LIBSSH
#include <libssh/callbacks.h>
#endif


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

bool SshConnectionPool::hasBrokenHybridKex(const QString& runtimeVersion)
{
    // ssh_version() appends the crypto/compression backends after the release,
    // so compare the leading version token rather than the whole string.
    const QString release =
        runtimeVersion.section(QLatin1Char('/'), 0, 0).trimmed();
    return release == QLatin1String("0.12.0");
}

SshConnectionPool::AuthRung SshConnectionPool::nextAuthRung(
    const AuthRungsTried& tried, AuthMethods offered, bool canPrompt)
{
    // Public-key rungs come first because they need nothing from the user: an
    // agent key or an unencrypted key file authenticates silently. Only when
    // those are spent is a human interrupted, and the password rung is last
    // because it is the only secret that travels to the server.
    if (offered.publicKey) {
        if (!tried.agent)
            return AuthRung::Agent;
        if (!tried.keyFile)
            return AuthRung::KeyFile;
        if (canPrompt && !tried.keyPassphrase)
            return AuthRung::KeyPassphrase;
    }
    if (offered.password && canPrompt && !tried.password)
        return AuthRung::Password;
    return AuthRung::Exhausted;
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

void SshConnectionPool::clearDiagnostics()
{
    if (m_diagnosticLog.isEmpty())
        return;
    m_diagnosticLog.clear();
    emit diagnosticLogChanged();
}

void SshConnectionPool::appendDiagnostic(const QString& message)
{
    constexpr qsizetype maximumCharacters = 64 * 1024;
    const QString line = message.trimmed();
    if (line.isEmpty())
        return;

    if (!m_diagnosticLog.isEmpty())
        m_diagnosticLog += QLatin1Char('\n');
    m_diagnosticLog += line;
    if (m_diagnosticLog.size() > maximumCharacters) {
        m_diagnosticLog.remove(0, m_diagnosticLog.size() - maximumCharacters);
        m_diagnosticLog.prepend(
            QStringLiteral("… earlier SSH diagnostics discarded …\n"));
    }
    emit diagnosticLogChanged();
}

void SshConnectionPool::libsshLog(int priority, const char* function,
                                  const char* buffer, void* userdata)
{
    auto* const pool = static_cast<SshConnectionPool*>(userdata);
    if (!pool || !buffer)
        return;
    pool->appendDiagnostic(
        QStringLiteral("libssh[%1] %2: %3")
            .arg(priority)
            .arg(QString::fromUtf8(function ? function : "unknown"),
                 QString::fromUtf8(buffer)));
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

// Overwrite a secret in place before its buffer is handed back to the
// allocator, so a passphrase or password does not linger in freed heap memory
// for the rest of the process's lifetime (SPEC 12.3). Only the locally owned
// UTF-8 copy can be scrubbed: the QString it was converted from is
// reference-counted and shared with the caller that supplied it.
void wipeSecret(QByteArray& secret)
{
    if (!secret.isEmpty())
        secret.fill('\0');
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

// The method name for a rung, for diagnostics and the failure explanation.
// Names only — a rung never carries its secret into a message.
QString authRungName(SshConnectionPool::AuthRung rung)
{
    switch (rung) {
    case SshConnectionPool::AuthRung::Agent:
        return QStringLiteral("ssh-agent");
    case SshConnectionPool::AuthRung::KeyFile:
        return QStringLiteral("private key");
    case SshConnectionPool::AuthRung::KeyPassphrase:
        return QStringLiteral("private key (with passphrase)");
    case SshConnectionPool::AuthRung::Password:
        return QStringLiteral("password");
    case SshConnectionPool::AuthRung::Exhausted:
        break;
    }
    return QStringLiteral("none");
}

SshConnectionPool::AuthOutcome authenticateIdentityFile(
    ssh_session session, const QString& identityFile, const QString& passphrase)
{
    const QByteArray fileName = QFile::encodeName(identityFile);
    QByteArray passphraseUtf8 = passphrase.toUtf8();
    ssh_key privateKey = nullptr;
    const int importResult = ssh_pki_import_privkey_file(
        fileName.constData(),
        passphrase.isEmpty() ? nullptr : passphraseUtf8.constData(),
        declineLibsshPassphrase, nullptr, &privateKey);
    // libssh has copied everything it needs out of the buffer by now.
    wipeSecret(passphraseUtf8);
    if (importResult != SSH_OK || !privateKey) {
        // libssh does not hand back a key on failure, but freeing a non-null one
        // costs nothing and closes the leak if a future release ever did.
        if (privateKey)
            ssh_key_free(privateKey);
        return SshConnectionPool::AuthOutcome::Refused;
    }

    const int authenticationResult =
        ssh_userauth_publickey(session, nullptr, privateKey);
    ssh_key_free(privateKey);
    return SshConnectionPool::classifyAuthResult(authenticationResult);
}

// Stops at the first key the server did not reject: a partial success is
// progress and must not be followed by another key on the same rung, because
// the server is now asking for a DIFFERENT method.
SshConnectionPool::AuthOutcome authenticateIdentityFiles(
    ssh_session session, const QStringList& identityFiles,
    const QString& passphrase)
{
    for (const QString& identityFile : identityFiles) {
        const SshConnectionPool::AuthOutcome outcome =
            authenticateIdentityFile(session, identityFile, passphrase);
        if (outcome != SshConnectionPool::AuthOutcome::Refused)
            return outcome;
    }
    return SshConnectionPool::AuthOutcome::Refused;
}

} // namespace

SshConnectionPool::AuthMethods SshConnectionPool::methodsFromMask(
    int userauthListMask)
{
    const unsigned int mask = static_cast<unsigned int>(userauthListMask);
    if (mask == SSH_AUTH_METHOD_UNKNOWN) {
        // The server did not say. Try both rather than nothing: this is exactly
        // what the single-step ladder did before, and refusing to try anything
        // here would turn a missing method list into a connection that cannot
        // authenticate at all.
        return AuthMethods{true, true};
    }
    AuthMethods offered;
    offered.publicKey = (mask & SSH_AUTH_METHOD_PUBLICKEY) != 0;
    offered.password = (mask & SSH_AUTH_METHOD_PASSWORD) != 0;
    return offered;
}

SshConnectionPool::AuthOutcome SshConnectionPool::classifyAuthResult(
    int libsshResult)
{
    // SSH_AUTH_PARTIAL is the whole point of this function: the method WAS
    // accepted and the server wants another one. Everything that is neither
    // success nor partial (denied, error, again, keyboard-interactive info) ends
    // this rung — SSH_AUTH_AGAIN cannot occur here because the session is
    // blocking, and this client never requests keyboard-interactive.
    switch (libsshResult) {
    case SSH_AUTH_SUCCESS:
        return AuthOutcome::Granted;
    case SSH_AUTH_PARTIAL:
        return AuthOutcome::Partial;
    default:
        return AuthOutcome::Refused;
    }
}

bool SshConnectionPool::connectToHost(const QString& host, quint16 port,
                                      const QString& user,
                                      const QString& identityFile)
{
    clearDiagnostics();
    appendDiagnostic(QStringLiteral("Starting SSH connection to %1:%2.")
                         .arg(host)
                         .arg(port));

    disconnectFromHost();
    m_host = host;
    m_port = port;
    m_user = user;
    m_identityFile = resolveIdentityFilePath(identityFile);

    setState(State::Connecting);
    m_session = ssh_new();
    if (!m_session) {
        appendDiagnostic(QStringLiteral("libssh could not allocate a session."));
        emit errorOccurred(QStringLiteral("ssh_new() failed"));
        setState(State::Error);
        return false;
    }

    // libssh exposes only a process-global logging callback. SSH handshakes
    // here are synchronous, so restore the pre-existing callback and level
    // before this attempt returns.
    const ssh_logging_callback previousLogCallback = ssh_get_log_callback();
    void* const previousLogUserdata = ssh_get_log_userdata();
    const int previousLogLevel = ssh_get_log_level();
    ssh_set_log_callback(&SshConnectionPool::libsshLog);
    ssh_set_log_userdata(this);
    ssh_set_log_level(SSH_LOG_FUNCTIONS);
    const auto restoreLibsshLogging = qScopeGuard([previousLogCallback,
                                                    previousLogUserdata,
                                                    previousLogLevel] {
        ssh_set_log_level(previousLogLevel);
        ssh_set_log_userdata(previousLogUserdata);
        ssh_set_log_callback(previousLogCallback);
    });

    // Option failures are invisible in libssh's later return values: a private
    // key that never reached the session looks exactly like a key the server
    // rejected. Record them instead of dropping them on the floor.
    const auto setOption = [this](ssh_options_e option, const void* value,
                                  const QString& what) {
        if (ssh_options_set(m_session, option, value) == SSH_OK)
            return;
        appendDiagnostic(
            QStringLiteral("Could not apply %1: %2")
                .arg(what, QString::fromUtf8(ssh_get_error(m_session))));
    };

    const QString runtimeVersion = QString::fromUtf8(ssh_version(0));
    appendDiagnostic(
        QStringLiteral("libssh runtime: %1").arg(runtimeVersion));
    const QByteArray hostUtf8 = host.toUtf8();
    const QByteArray userUtf8 = user.toUtf8();
    unsigned int portValue = port;
    setOption(SSH_OPTIONS_HOST, hostUtf8.constData(),
              QStringLiteral("SSH host"));

    // libssh only learns IdentityFile, ProxyJump and the rest of the user's
    // OpenSSH configuration when asked to parse it. Set Host first so its
    // `Host` blocks match, then re-apply the explicitly saved user/port below:
    // profile values deliberately win over broad config defaults.
    const QString configPath =
        QDir::home().filePath(QStringLiteral(".ssh/config"));
    if (QFileInfo(configPath).isFile()) {
        appendDiagnostic(
            QStringLiteral("Parsing SSH configuration: %1").arg(configPath));
        const QByteArray configUtf8 = QFile::encodeName(configPath);
        if (ssh_options_parse_config(m_session, configUtf8.constData())
            != SSH_OK) {
            const QString error = QString::fromUtf8(ssh_get_error(m_session));
            appendDiagnostic(
                QStringLiteral("SSH configuration parsing failed: %1")
                    .arg(error));
            emit errorOccurred(
                QStringLiteral("Could not parse SSH config %1: %2")
                    .arg(configPath, error));
            closeSession();
            setState(State::Error);
            return false;
        }
    }

    setOption(SSH_OPTIONS_PORT, &portValue, QStringLiteral("SSH port"));
    setOption(SSH_OPTIONS_USER, userUtf8.constData(),
              QStringLiteral("SSH user"));
    if (!m_identityFile.isEmpty()) {
        const QByteArray identityUtf8 = QFile::encodeName(m_identityFile);
        setOption(SSH_OPTIONS_IDENTITY, identityUtf8.constData(),
                  QStringLiteral("private key file %1").arg(m_identityFile));
    }

    // libssh 0.12.0's mlkem768x25519-sha256 branch hands ssh_buffer_pack() an
    // un-cast `int` where it reads a `size_t`, so packing the client KEX init
    // can fail on "Failed to construct client init buffer" before a host key is
    // ever seen. That algorithm leads libssh's DEFAULT_KEY_EXCHANGE and modern
    // sshd prefers it too, so the very first attempt hits it. Upstream fixed
    // the cast in 0.12.1. Remove ONLY that algorithm: libssh's `-` modifier
    // subtracts from its OWN DEFAULT_KEY_EXCHANGE, so no list is frozen here and
    // the NIST hybrid keeps post-quantum key exchange available. It does replace
    // (not filter) any KexAlgorithms parsed from ~/.ssh/config above - an
    // acceptable trade against a handshake that cannot complete at all.
    if (hasBrokenHybridKex(runtimeVersion)) {
        const bool applied = ssh_options_set(m_session,
                                             SSH_OPTIONS_KEY_EXCHANGE,
                                             "-mlkem768x25519-sha256")
                             == SSH_OK;
        appendDiagnostic(
            applied
                ? QStringLiteral("Disabled mlkem768x25519-sha256: libssh %1 "
                                 "cannot pack its client KEX init.")
                      .arg(runtimeVersion)
                : QStringLiteral("Could not disable mlkem768x25519-sha256 on "
                                 "libssh %1: %2")
                      .arg(runtimeVersion,
                           QString::fromUtf8(ssh_get_error(m_session))));
    }

    appendDiagnostic(QStringLiteral("Beginning SSH handshake."));
    if (ssh_connect(m_session) != SSH_OK) {
        const QString error = QString::fromUtf8(ssh_get_error(m_session));
        appendDiagnostic(QStringLiteral("SSH handshake failed: %1").arg(error));
        if (hasBrokenHybridKex(runtimeVersion)
            && error.contains(
                QStringLiteral("Failed to construct client init buffer"),
                Qt::CaseInsensitive)) {
            appendDiagnostic(
                QStringLiteral("Remediation: libssh %1 cannot pack a hybrid "
                               "ML-KEM client KEX init. Use a CodeHarbor build "
                               "linked with libssh 0.12.1 or newer.")
                    .arg(runtimeVersion));
        }
        emit errorOccurred(error);
        closeSession();
        setState(State::Error);
        return false;
    }

    appendDiagnostic(
        QStringLiteral("SSH handshake completed; verifying host key."));
    setState(State::HostKeyCheck);
    if (!verifyHostKey(host)) {
        const QString libsshError =
            QString::fromUtf8(ssh_get_error(m_session)).trimmed();
        // The refusal is CodeHarbor's own policy decision, so libssh normally
        // has nothing to add here; appending an empty or stale message would
        // only make the transcript read as though libssh had failed.
        appendDiagnostic(libsshError.isEmpty()
                             ? QStringLiteral("Host-key verification failed.")
                             : QStringLiteral("Host-key verification failed: %1")
                                   .arg(libsshError));
        closeSession();
        setState(State::Error);
        return false;
    }

    appendDiagnostic(QStringLiteral("Host key accepted; authenticating."));
    setState(State::Authenticating);
    if (!authenticate(user)) {
        const QString error = authenticationFailure();
        appendDiagnostic(QStringLiteral("SSH authentication failed: %1")
                             .arg(error));
        emit errorOccurred(error);
        closeSession();
        setState(State::Error);
        return false;
    }

    appendDiagnostic(QStringLiteral("SSH authentication succeeded."));
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
    // Detach the session from the member BEFORE announcing the teardown: a slot
    // reached from sessionClosing() that asks for another disconnect must not
    // start a second teardown of the same session, and nothing may open a fresh
    // channel on a session that is going away. A nested call now sees no session
    // and returns.
    const ssh_session session = m_session;
    m_session = nullptr;
    // Anything still holding one of our channel handles has to let go BEFORE
    // the handles are freed, or its next libssh call reads freed memory.
    // Announced first because a handler answers by calling releaseChannel(),
    // which mutates m_channels — so the sweep below works on what is left once
    // that has settled.
    emit sessionClosing();
    // Channels are attached to the session; ssh_free() would free them too, so
    // free them explicitly FIRST to keep a clear ownership boundary and avoid a
    // double-free/UAF if a caller still holds a (now-invalid) handle.
    const QList<ssh_channel> channels = m_channels;
    m_channels.clear();
    for (ssh_channel channel : channels) {
        if (!channel)
            continue;
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
    if (ssh_is_connected(session))
        ssh_disconnect(session);
    ssh_free(session);
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
    // field); decode it to the raw blob KnownHosts stores and compares. Strict
    // decoding on purpose: a lenient decode silently skips invalid characters
    // and would hand KnownHosts a blob that is not what the server presented.
    char* b64Key = nullptr;
    if (ssh_pki_export_pubkey_base64(serverKey, &b64Key) != SSH_OK || !b64Key) {
        ssh_key_free(serverKey);
        emit errorOccurred(QStringLiteral("Could not export server host key"));
        return false;
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        QByteArray(b64Key),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    ssh_string_free_char(b64Key);
    ssh_key_free(serverKey);

    // A key type libssh cannot name, or a blob that will not decode, cannot be
    // compared against the store — and must NOT be treated as first use.
    // verify() would answer Unknown, the user would get the reassuring
    // trust-this-host prompt, and add() would persist a line that reparses as
    // garbage (an empty key type collapses two fields into one), silently
    // losing the trust on the next launch.
    const QByteArray keyBlob = decoded ? decoded.decoded : QByteArray();
    if (keyType.isEmpty() || keyBlob.isEmpty()) {
        emit errorOccurred(
            QStringLiteral("Server host key could not be read as a comparable "
                           "key — refusing connection"));
        return false;
    }

    // Non-default ports are stored OpenSSH-style as "[host]:port"; match that
    // form so a ported entry is found (and persisted) correctly. m_port, not
    // whatever ~/.ssh/config asked for: connectToHost() re-applies the profile's
    // port after parsing the config, so this is the port actually connected to.
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
    m_partialMethods.clear();
    m_publicKeyOffered = false;

    // ssh_userauth_list() is only defined after a "none" request. It is not
    // enough to query it after an auto-key failure: some libssh builds then
    // report no methods at all, silently skipping the passphrase callback.
    if (ssh_userauth_none(m_session, nullptr) == SSH_AUTH_SUCCESS)
        return true;

    // Windows' built-in OpenSSH agent is a named pipe, but libssh expects an
    // AF_UNIX socket. Avoid every libssh auto-auth call in that case: it
    // retries the agent internally and leaves public-key fallback unusable.
    const bool unsupportedWindowsAgent = usesUnsupportedWindowsAgent();
    const QStringList identityFiles =
        identityFileCandidates(m_session, m_identityFile);

    // Authentication is a CONVERSATION, not a list of fallbacks. A server with
    // `AuthenticationMethods publickey,password` answers an accepted key with
    // SSH_AUTH_PARTIAL and then advertises only the method it still wants, so
    // the ladder is re-derived from the server's current offer after every step
    // rather than walked blindly. It terminates because nextAuthRung() never
    // returns a rung already in `tried`: at most four steps, however many
    // partial successes the server reports.
    AuthMethods offered = methodsFromMask(ssh_userauth_list(m_session, nullptr));
    m_publicKeyOffered = offered.publicKey;
    AuthRungsTried tried;
    const bool canPrompt = static_cast<bool>(m_credentialCallback);

    for (AuthRung rung = nextAuthRung(tried, offered, canPrompt);
         rung != AuthRung::Exhausted;
         rung = nextAuthRung(tried, offered, canPrompt)) {
        // Marked spent BEFORE the attempt, so no path out of the switch below
        // can leave the same rung eligible again.
        tried.add(rung);
        AuthOutcome outcome = AuthOutcome::Refused;

        switch (rung) {
        case AuthRung::Agent:
            if (!unsupportedWindowsAgent) {
                outcome =
                    classifyAuthResult(ssh_userauth_agent(m_session, nullptr));
            }
            break;

        case AuthRung::KeyFile:
            outcome =
                unsupportedWindowsAgent
                    ? authenticateIdentityFiles(m_session, identityFiles,
                                                QString())
                    : classifyAuthResult(ssh_userauth_publickey_auto(
                          m_session, nullptr, nullptr));
            break;

        case AuthRung::KeyPassphrase: {
            // With no key to unlock there is nothing a passphrase could do, and
            // asking for one would be a prompt the user cannot satisfy.
            if (unsupportedWindowsAgent && identityFiles.isEmpty())
                break;
            // A passphrase is used ONLY for public-key authentication: handing
            // it to ssh_userauth_password() would disclose a local key secret to
            // the remote server and make the two credential classes
            // indistinguishable. The password rung below asks separately.
            CredentialReply passphrase =
                m_credentialCallback(user, CredentialKind::KeyPassphrase);
            if (passphrase.promptRequested) {
                // The pool never blocks on the user: the attempt is abandoned
                // here, the controller raises the prompt, and the whole connect
                // is retried with the secret in hand (SPEC 12.1).
                appendDiagnostic(QStringLiteral(
                    "A private-key passphrase is required; abandoning this "
                    "attempt so it can be requested."));
                return false;
            }
            if (passphrase.secret.isEmpty())
                break;
            if (unsupportedWindowsAgent) {
                outcome = authenticateIdentityFiles(m_session, identityFiles,
                                                    passphrase.secret);
            } else {
                QByteArray secretUtf8 = passphrase.secret.toUtf8();
                const int result = ssh_userauth_publickey_auto(
                    m_session, nullptr, secretUtf8.constData());
                wipeSecret(secretUtf8);
                outcome = classifyAuthResult(result);
            }
            break;
        }

        case AuthRung::Password: {
            // An independent, opt-in credential. A GUI already parked on the
            // passphrase request is never overwritten by this one, because the
            // request above returns from the whole handshake.
            CredentialReply password =
                m_credentialCallback(user, CredentialKind::Password);
            if (password.promptRequested) {
                appendDiagnostic(QStringLiteral(
                    "A password is required; abandoning this attempt so it can "
                    "be requested."));
                return false;
            }
            if (password.secret.isEmpty())
                break;
            QByteArray secretUtf8 = password.secret.toUtf8();
            const int result = ssh_userauth_password(m_session, nullptr,
                                                    secretUtf8.constData());
            wipeSecret(secretUtf8);
            outcome = classifyAuthResult(result);
            break;
        }

        case AuthRung::Exhausted:
            break;  // unreachable: the loop condition excludes it
        }

        if (outcome == AuthOutcome::Granted)
            return true;
        if (outcome == AuthOutcome::Partial) {
            m_partialMethods << authRungName(rung);
            appendDiagnostic(
                QStringLiteral("The server accepted %1 and requires a further "
                               "authentication method.")
                    .arg(authRungName(rung)));
        }
        // Re-read after every step. The offer belongs to the SERVER and changes
        // as the exchange proceeds — after a partial success it typically drops
        // the method just satisfied — so a stale copy would keep offering a
        // method the server has stopped asking for.
        offered = methodsFromMask(ssh_userauth_list(m_session, nullptr));
    }
    return false;
}

QString SshConnectionPool::authenticationFailure() const
{
    QStringList details;
    details << QStringLiteral("Authentication failed.");

    // A partial success is a completely different failure from "nothing
    // worked": the server ACCEPTED what CodeHarbor sent and is asking for a
    // further method, so the ssh-agent and identity-file advice below would be
    // actively misleading here. Method names only — never a secret.
    if (!m_partialMethods.isEmpty()) {
        details << QStringLiteral(
                       "The server accepted %1 and then required a further "
                       "authentication method that CodeHarbor could not "
                       "supply. Check the server's AuthenticationMethods "
                       "setting: a private key combined with a password is "
                       "supported, keyboard-interactive is not.")
                       .arg(m_partialMethods.join(QStringLiteral(", ")));
        const QString partialLibsshError =
            m_session ? QString::fromUtf8(ssh_get_error(m_session)).trimmed()
                      : QString();
        if (!partialLibsshError.isEmpty())
            details << QStringLiteral("libssh: %1.").arg(partialLibsshError);
        return details.join(QLatin1Char(' '));
    }

    // A server that never offered public-key authentication was never given a
    // key, so advice about ssh-agent and identity files describes something that
    // did not happen. Say what actually did.
    if (!m_publicKeyOffered) {
        details << QStringLiteral(
            "The server does not accept public-key authentication, so only its "
            "password method could be tried.");
        const QString passwordOnlyError =
            m_session ? QString::fromUtf8(ssh_get_error(m_session)).trimmed()
                      : QString();
        if (!passwordOnlyError.isEmpty())
            details << QStringLiteral("libssh: %1.").arg(passwordOnlyError);
        return details.join(QLatin1Char(' '));
    }

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

void SshConnectionPool::releaseChannel(ssh_channel channel)
{
    if (!channel)
        return;
    // removeOne() IS the double-release guard: the second call finds nothing to
    // remove and returns without touching freed memory.
    if (!m_channels.removeOne(channel))
        return;
    if (ssh_channel_is_open(channel))
        ssh_channel_close(channel);
    ssh_channel_free(channel);
}

#endif // CH_HAVE_LIBSSH

} // namespace ch
