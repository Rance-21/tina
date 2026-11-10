#pragma once

#include <QObject>
#include <QStringConverter>
#include <QTimer>
#include <array>
#include <QtQmlIntegration/qqmlintegration.h>

namespace tina::ui {

class FfiClientBridge;

// 每个 Shell 独占固定容量的字符环；包括没有换行的输出，也不会无限增长。
class TerminalSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Terminals are created by FfiClientBridge")
    Q_PROPERTY(int sessionId READ sessionId CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(quint32 terminalId READ terminalId NOTIFY stateChanged)
    Q_PROPERTY(QString output READ output NOTIFY outputChanged)
    Q_PROPERTY(QString draft READ draft WRITE setDraft NOTIFY draftChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool starting READ starting NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool completing READ completing NOTIFY stateChanged)
    Q_PROPERTY(bool submitting READ submitting NOTIFY stateChanged)

  public:
    static constexpr qsizetype BufferCapacity = 128 * 1024; // 256 KiB UTF-16
    static constexpr int MaxLines = 2000;
    explicit TerminalSession(int sessionId, QObject *parent = nullptr);
    int sessionId() const { return sessionId_; }
    QString title() const { return QStringLiteral("Terminal %1").arg(sessionId_); }
    quint32 terminalId() const { return terminalId_; }
    QString output() const;
    QString draft() const { return draft_; }
    void setDraft(const QString &draft);
    bool available() const { return available_; }
    bool starting() const { return starting_; }
    bool busy() const { return busy_; }
    bool completing() const { return completing_; }
    bool submitting() const { return submitting_; }
    void appendOutput(QByteArrayView bytes);
    Q_INVOKABLE void clear();

  signals:
    void stateChanged();
    void outputChanged();
    void draftChanged();

  private:
    friend class FfiClientBridge;
    enum class Escape { None, Start, Csi, Osc, OscEnd, Charset };
    void put(QChar ch);
    void dropFirst();
    void finishCsi(QChar final);
    void scheduleOutput();
    std::array<QChar, BufferCapacity> buffer_{};
    quint64 first_{};
    quint64 end_{};
    quint64 lineStart_{};
    quint64 cursor_{};
    int lines_{};
    QStringDecoder decoder_{QStringDecoder::Utf8};
    Escape escape_{Escape::None};
    QString parameters_;
    QTimer outputTimer_;
    const int sessionId_;
    QString draft_;
    quint64 draftRevision_{};
    quint32 terminalId_{};
    bool available_{};
    bool starting_{};
    bool busy_{};
    bool completing_{};
    bool submitting_{};
};

} // namespace tina::ui
