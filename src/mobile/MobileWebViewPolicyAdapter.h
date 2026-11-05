#pragma once

#include <QString>
#include <QUrl>

#include <memory>

class QWindow;

namespace ch {

enum class MobileWebViewPolicyState { Pending, Ready, Failed };

struct MobileWebViewPolicyResult {
    MobileWebViewPolicyState state = MobileWebViewPolicyState::Pending;
    QString error;
};

// One installation session for one app-created native view. Factory, poll and
// destruction run on the Qt GUI thread; platform UI work may finish later.
// Match the exact unique about:blank marker, never the first native web view.
// Ready means both navigation and new-window policy are already installed.
// Permit http(s) navigation inside this view; reject other page-controlled
// navigation schemes before Qt's default delegate can open them externally.
// Route http(s) new windows to this view. The exact marker is an internal
// bootstrap URL only; local file/content resources must never load. Safe inline
// data/blob subresources may remain available without permitting such navigation.
//
// Destruction cancels pending work. A surviving identified view must remain
// fail-closed: stop loading and block untrusted navigation, without restoring an
// unsafe default delegate. Native jobs/callbacks must not retain a raw C++ owner.
// Platform state is scoped to this session/native view, not a permanent registry.
class MobileWebViewPolicyAdapter {
public:
    virtual ~MobileWebViewPolicyAdapter() = default;
    virtual MobileWebViewPolicyResult poll() = 0;
};

std::unique_ptr<MobileWebViewPolicyAdapter> createMobileWebViewPolicyAdapter(
    QWindow *window, const QUrl &marker);

} // namespace ch
