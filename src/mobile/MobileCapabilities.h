#pragma once

#include <QObject>
#include <QQmlEngine>

namespace ch {

// Which OPTIONAL Qt modules this build actually has, as two booleans QML can
// bind to.
//
// The mobile shell has no WebEngine, so PDF and web pages are the two surfaces
// that depend on a Qt module which may simply not be present in the kit the
// binary was built against: Qt Pdf and Qt WebView are separate, individually
// installable modules, and the Qt for Android / Qt for iOS installers let a user
// deselect either. CMake probes for them (CH_HAVE_QTPDF / CH_HAVE_QTWEBVIEW) and
// this object is the ONE place that translates the compile-time answer into
// something a QML binding can read.
//
// Why it matters that QML asks rather than tries: a `import QtQuick.Pdf` in a
// page that is loaded on a kit without the module is a QML ERROR, and a Loader
// reports it as a blank pane with a message in the log the user will never see.
// PaneHostPage therefore chooses ViewerUnsupportedPage up front, so a pdf pane
// on a PDF-less build says so in words instead of showing nothing.
//
// Both properties are CONSTANT: they describe how the binary was compiled and
// cannot change while it runs.
class MobileCapabilities : public QObject {
    Q_OBJECT
    // Reached as mobile.capabilities; registered so QML can resolve the type
    // of that property rather than treating it as an opaque QObject.
    QML_ELEMENT
    QML_UNCREATABLE("MobileCapabilities is owned by MobileAppController.")
    Q_PROPERTY(bool hasPdf READ hasPdf CONSTANT)
    Q_PROPERTY(bool hasWebView READ hasWebView CONSTANT)

public:
    explicit MobileCapabilities(QObject* parent = nullptr);

    bool hasPdf() const;
    bool hasWebView() const;
};

} // namespace ch
