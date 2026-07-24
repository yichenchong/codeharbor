#include "SshConnectionPool.h"

namespace ch {

bool SshConnectionPool::libsshAvailable()
{
#if CH_HAVE_LIBSSH
    return true;
#else
    return false;
#endif
}

} // namespace ch
