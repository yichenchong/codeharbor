#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

// Brings ch::CodeharbordClient's full definition with it, which the QPointer
// member below needs.
#include "EditorController.h"

namespace ch {

// Per-pane EditorController factory (SPEC 4.5 split panes). The text editor is
// one of the VIEWER pane's handlers (SPEC 8.1), not a pane type of its own, so
// "per-pane" here means per viewer pane: each one that reaches the text-editor
// handler must own its OWN controller, because a single shared controller would
// let split panes clobber each other's path/revision/file-state. Exposed to QML
// as the `editorFactory` context property; create() returns a controller
// parented to the requesting pane so it is destroyed with the pane, and its
// server-side file.watch subscription is released with it rather than leaked
// (SPEC 8.7).
class EditorFactory : public QObject {
    Q_OBJECT
public:
    explicit EditorFactory(CodeharbordClient* client, QObject* parent = nullptr);

    // Create a controller owned by `owner` (the QML pane Item). Ownership is via
    // QObject parenting, so the controller — and the file.watch subscription it
    // holds — is released when the pane is destroyed. `paneId` is the pane's
    // stable layout id, used to key its crash-recovery snapshot per pane so two
    // panes editing one path never collide (SPEC 11.3). A NULL `owner` parents
    // the controller to the factory instead of returning it unparented; see
    // create() for why that matters to a Q_INVOKABLE.
    Q_INVOKABLE ch::EditorController* create(QObject* owner = nullptr,
                                             const QString& paneId = QString());

    // The server-reported recovery directory (server.info.recoveryDir),
    // forwarded by AppController once the server identity is adopted. Remembered
    // for controllers minted later and pushed onto the live ones now, so a
    // recoveryDir that answers AFTER panes exist still reaches them (SPEC 11.3).
    void setRecoveryDir(QString dir);

private:
    // QPointer for the same reason EditorController holds one: the client is
    // owned by the caller and can be destroyed while this factory (a QML
    // context property that lives as long as the process) still exists. A
    // controller minted afterwards would otherwise connect to freed memory;
    // with a QPointer it is simply created clientless and does nothing.
    QPointer<CodeharbordClient> m_client = nullptr;
    QString m_recoveryDir;
    // Every controller minted, guarded so a pane destroyed since is pruned
    // rather than dereferenced when setRecoveryDir() fans a late value out.
    QList<QPointer<EditorController>> m_controllers;
};

} // namespace ch
