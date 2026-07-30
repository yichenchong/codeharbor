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
    // Parented to the pane: destroyed with it, releasing its file.watch.
    auto* controller = new EditorController(m_client, paneId, owner);
    controller->setRecoveryDir(m_recoveryDir);
    // Track it so a recoveryDir arriving later (server.info is asynchronous and
    // may answer after panes exist) still reaches controllers already created.
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
