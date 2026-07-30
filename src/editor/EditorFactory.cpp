#include "EditorFactory.h"

#include "EditorController.h"

namespace ch {

EditorFactory::EditorFactory(CodeharbordClient* client, QObject* parent)
    : QObject(parent), m_client(client)
{
}

EditorController* EditorFactory::create(QObject* owner)
{
    // Parented to the pane: destroyed with it, releasing its file.watch.
    return new EditorController(m_client, owner);
}

} // namespace ch
