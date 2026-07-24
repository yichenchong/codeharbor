#include "WorkspaceDb.h"

namespace ch {

QString WorkspaceDb::schemaVersionString()
{
    return QString::number(kSchemaVersion);
}

} // namespace ch
