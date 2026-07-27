#include "EditorFactory.h"

#include "EditorController.h"

namespace ch {

EditorFactory::EditorFactory(CodeharbordClient* client, QObject* parent)
    : QObject(parent), m_client(client)
{
}

EditorController* EditorFactory::create(QObject* owner)
{
    // Parented to the pane: destroyed with it (no leaked watches/recovery).
    return new EditorController(m_client, owner);
}

} // namespace ch
