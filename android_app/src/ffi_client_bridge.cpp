
#include "ffi_client_bridge.hpp"

#include <QAbstractSocket>
#include <QtEndian>

namespace tina::ui {

FfiClientBridge::FfiClientBridge(QObject *parent) : QObject(parent) {

    connect(&socket_, &QTcpSocket::connected, this, [this] {
        emit connectionStateChanged(true);
        ping();
    });

    connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        pendingPingId_ = 0;
        emit connectionStateChanged(false);
    });

    connect(&socket_, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit pingResult(false, socket_.errorString());
    });

    connect(&socket_, &QTcpSocket::readyRead, this, &FfiClientBridge::onReadyRead);
}

void FfiClientBridge::connectToServer(const QString &host, int port) {
    if (host.isEmpty() || port <= 0 || port > 65535) {
        emit pingResult(false, QStringLiteral("Invalid host or port"));
        return;
    }

    socket_.abort();
    input_.clear();
    pendingPingId_ = 0;

    socket_.connectToHost(host, static_cast<quint16>(port));
}

void FfiClientBridge::ping() {
    if (socket_.state() != QAbstractSocket::ConnectedState) {
        emit pingResult(false, QStringLiteral("Not connected"));
        return;
    }

    // Tina RPC 固定帧头：
    // Length(4) + Kind(1) + ID(8) + Method(2) + Status(2)
    // Ping 没有 Payload，Length = 13。
    QByteArray frame(17, '\0');

    qToBigEndian<quint32>(13, reinterpret_cast<uchar *>(frame.data()));

    frame[4] = 0x01; // Request

    const quint64 id = nextId_++;
    pendingPingId_ = id;

    qToBigEndian<quint64>(id, reinterpret_cast<uchar *>(frame.data() + 5));

    qToBigEndian<quint16>(0x0001, reinterpret_cast<uchar *>(frame.data() + 13));

    // Status = 0，frame 初始化时已清零。
    socket_.write(frame);
}

void FfiClientBridge::onReadyRead() {
    input_.append(socket_.readAll());

    while (input_.size() >= 4) {
        const auto *data = reinterpret_cast<const uchar *>(input_.constData());

        const quint32 length = qFromBigEndian<quint32>(data);

        if (length < 13 || length > 1024 * 1024) {
            emit pingResult(false, QStringLiteral("Invalid RPC frame"));
            socket_.abort();
            input_.clear();
            return;
        }

        const qsizetype total = 4 + length;
        if (input_.size() < total)
            return; // TCP 半包，等待下一次 readyRead。

        const quint8 kind = data[4];
        const quint64 id = qFromBigEndian<quint64>(data + 5);
        const quint16 method = qFromBigEndian<quint16>(data + 13);
        const quint16 status = qFromBigEndian<quint16>(data + 15);

        const QByteArray payload(input_.constData() + 17, static_cast<qsizetype>(length - 13));

        input_.remove(0, total);

        if (kind != 0x02 || method != 0x0001 || id != pendingPingId_)
            continue;

        pendingPingId_ = 0;

        const bool ok = status == 0 && payload == "PONG";
        emit pingResult(ok, QString::fromUtf8(payload));
    }
}

void FfiClientBridge::submitCommand(const QString &command) {
    emit commandSubmitted(command);
}

void FfiClientBridge::submitAgentInput(const QString &input) {
    if (input.trimmed().compare(QStringLiteral("@AI"), Qt::CaseInsensitive) == 0) {
        emit openAgentWindowRequested();
    }
}

} // namespace tina::ui
