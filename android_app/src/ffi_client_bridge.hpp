
#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace tina::ui {

class FfiClientBridge : public QObject {
    Q_OBJECT
    QML_ELEMENT

  public:
    explicit FfiClientBridge(QObject *parent = nullptr);

    Q_INVOKABLE void connectToServer(const QString &host, int port = 3490);
    Q_INVOKABLE void ping();

    // 保留现有 QML 需要的接口
    Q_INVOKABLE void submitCommand(const QString &command);
    Q_INVOKABLE void submitAgentInput(const QString &input);

  signals:
    void connectionStateChanged(bool connected);
    void pingResult(bool ok, const QString &message);

    void commandSubmitted(const QString &command);
    void openAgentWindowRequested();

  private:
    void onReadyRead();

    QTcpSocket socket_;
    QByteArray input_;
    quint64 nextId_{1};
    quint64 pendingPingId_{0};
};

} // namespace tina::ui
