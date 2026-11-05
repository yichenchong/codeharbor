#pragma once

#include "MobileWebViewPolicyAdapter.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <memory>

namespace ch {

// No untrusted address is loaded until a platform guard has claimed the native
// view carrying this instance's unique bootstrap marker.
class MobileWebViewPolicy : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl marker READ marker CONSTANT)
    Q_PROPERTY(bool ready READ ready NOTIFY statusChanged)
    Q_PROPERTY(bool installing READ installing NOTIFY statusChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY statusChanged)

public:
    explicit MobileWebViewPolicy(QObject *parent = nullptr);
    ~MobileWebViewPolicy() override;

    QUrl marker() const { return m_marker; }
    bool ready() const { return m_ready; }
    bool installing() const { return m_installing; }
    QString errorText() const { return m_error; }

    Q_INVOKABLE void attach(QQuickItem *view);

signals:
    void statusChanged();

private:
    void poll();
    void fail(const QString &message);

    const QUrl m_marker;
    QPointer<QQuickItem> m_view;
    QPointer<QWindow> m_window;
    std::unique_ptr<MobileWebViewPolicyAdapter> m_adapter;
    QTimer m_timer;
    QElapsedTimer m_elapsed;
    bool m_attached = false;
    bool m_ready = false;
    bool m_installing = false;
    QString m_error;
};

} // namespace ch
