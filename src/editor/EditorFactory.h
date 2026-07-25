#pragma once

#include <QObject>

#include "EditorController.h"

namespace ch {

class CodeharbordClient;

// Per-pane EditorController factory (SPEC 4.5 split panes). Each editor pane
// must own its OWN controller: a single shared controller would let split panes
// clobber each other's path/revision/file-state. Exposed to QML as the
// `editorFactory` context property; create() returns a controller parented to
// the requesting pane so it is destroyed with the pane (no leaked file watches
// or recovery timers).
class EditorFactory : public QObject {
    Q_OBJECT
public:
    explicit EditorFactory(CodeharbordClient* client, QObject* parent = nullptr);

    // Create a controller owned by `owner` (the QML pane Item). Ownership is via
    // QObject parenting, so the controller (and its watch/recovery state) is
    // released when the pane is destroyed.
    Q_INVOKABLE ch::EditorController* create(QObject* owner = nullptr);

private:
    CodeharbordClient* m_client = nullptr;
};

} // namespace ch
