#include "EditorFactory.h"

#include "EditorController.h"

#include <utility>

namespace ch {

EditorFactory::EditorFactory(CodeharbordClient* client, QObject* parent)
    : QObject(parent), m_client(client)
{
}

EditorController* EditorFactory::create(QObject* owner, const QString& paneId)
{
    // Parented to the pane: destroyed with it, releasing its file.watch. With no
    // pane to parent to, the factory itself takes ownership rather than returning
    // a free-floating QObject: this is a Q_INVOKABLE, so an unparented return
    // value reaches QML with JavaScriptOwnership and can be collected at a moment
    // nothing here controls — while a live pane is still driving it — and a C++
    // caller that ignored the default would simply leak it. Same rule and same
    // shape as TerminalFactory::create().
    auto* controller = new EditorController(m_client, paneId, owner ? owner : this);
    controller->setRecoveryDir(m_recoveryDir);
    // Track it so a recoveryDir arriving later (server.info is asynchronous and
    // may answer after panes exist) still reaches controllers already created.
    // Panes come and go for the lifetime of the process, so the entries whose
    // pane is already gone are dropped here too: pruning only in
    // setRecoveryDir() (which fires at most once per connect) lets this list
    // grow with every pane ever opened.
    m_controllers.removeIf(
        [](const QPointer<EditorController>& c) { return c.isNull(); });
    m_controllers.append(controller);
    return controller;
}

void EditorFactory::setRecoveryDir(QString dir)
{
    m_recoveryDir = std::move(dir);
    // Drop controllers whose pane has been destroyed, then push the new base
    // onto the survivors; recoveryPath() reads it lazily on the next snapshot.
    m_controllers.removeIf(
        [](const QPointer<EditorController>& c) { return c.isNull(); });
    for (const QPointer<EditorController>& c : m_controllers)
        c->setRecoveryDir(m_recoveryDir);
}

} // namespace ch
